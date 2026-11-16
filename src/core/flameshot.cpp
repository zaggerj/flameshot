// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "flameshot.h"
#include "core/flameshotdaemon.h"
#include "utils/scrollcapture.h"
#include "utils/windowforeground.h"

#include <QTimer>
#if defined(Q_OS_MACOS)
#include "qhotkey.h"
#endif

#if defined(Q_OS_MACOS)
#include <QWindow>
#include <objc/message.h>

namespace {

constexpr long NSApplicationActivationPolicyRegular = 0;
constexpr long NSApplicationActivationPolicyAccessory = 1;

void setActivationPolicy(long policy)
{
    auto sharedApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto setPolicy = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    id nsApp = sharedApp(reinterpret_cast<id>(objc_getClass("NSApplication")),
                         sel_registerName("sharedApplication"));
    setPolicy(nsApp, sel_registerName("setActivationPolicy:"), policy);
}

void setActivationPolicyRegular()
{
    setActivationPolicy(NSApplicationActivationPolicyRegular);
}

void setActivationPolicyAccessory()
{
    setActivationPolicy(NSApplicationActivationPolicyAccessory);
}

constexpr const char* visibleInDockProperty = "_visibleInDock";

// NSMainMenuWindowLevel (24) + 2: above the menu bar, its status items and
// the Dock, but below pop-up menus so Qt menus remain visible.
constexpr long captureWindowLevel = 26;

// Qt::WindowStaysOnTopHint maps to NSFloatingWindowLevel, which is below the
// menu bar and the Dock, so they would cover parts of the capture overlay.
void raiseAboveMenuBar(QWidget* widget)
{
    auto getWindow = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto setLevel = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    auto nsView = reinterpret_cast<id>(widget->winId());
    id nsWindow = getWindow(nsView, sel_registerName("window"));
    if (nsWindow != nullptr) {
        setLevel(nsWindow, sel_registerName("setLevel:"), captureWindowLevel);
    }
}

} // namespace

#include <CoreGraphics/CoreGraphics.h>
#endif

#include "config/cacheutils.h"
#include "config/configresolver.h"
#include "config/configwindow.h"
#include "utils/translationmanager.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"
#include "utils/screengrabber.h"
#include "utils/screenshotsaver.h"
#include "widgets/capture/capturewidget.h"
#include "widgets/capturelauncher.h"
#include "widgets/infowindow.h"

#ifdef ENABLE_IMGUR
#include "tools/imgupload/imguploadermanager.h"
#include "tools/imgupload/storages/imguploaderbase.h"
#include "widgets/imguploaddialog.h"
#include "widgets/uploadhistory.h"
#endif

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#if defined(Q_OS_MACOS)
#include <QScreen>
#endif

Flameshot::Flameshot()
  : m_haveExternalWidget(false)
  , m_captureWindow(nullptr)
#if defined(Q_OS_MACOS)
  , m_HotkeyScreenshotCapture(nullptr)
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
  , m_HotkeyScreenshotHistory(nullptr)
#endif
{
    QString StyleSheet = CaptureButton::globalStyleSheet();
    qApp->setStyleSheet(StyleSheet);

    connect(TranslationManager::instance(),
            &TranslationManager::languageChanged,
            this,
            &Flameshot::onLanguageChanged);

#if defined(Q_OS_MACOS)
    // Request Screen Recording permission via the proper CoreGraphics API
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
    }
#endif
#if defined(Q_OS_MACOS)
    // Set global shortcuts for MacOS. Windows goes through
    // GlobalShortcutFilter: registering the same chord here as well made the
    // second RegisterHotKey fail (error 1409) in every capture process and
    // left a hotkey owner that was not the tray daemon.
    m_HotkeyScreenshotCapture = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("TAKE_SCREENSHOT")), true, this);
    QObject::connect(m_HotkeyScreenshotCapture,
                     &QHotkey::activated,
                     qApp,
                     [this]() { gui(); });

    // Apply global shortcut changes immediately instead of requiring a
    // restart. ConfigHandler emits fileChanged whenever the ini file is
    // written (including changes made from the settings dialog).
    QObject::connect(ConfigHandler::getInstance(),
                     &ConfigHandler::fileChanged,
                     this,
                     [this]() {
                         const QKeySequence sequence(
                           ConfigHandler().shortcut("TAKE_SCREENSHOT"));
                         if (m_HotkeyScreenshotCapture->shortcut() !=
                             sequence) {
                             m_HotkeyScreenshotCapture->setShortcut(sequence,
                                                                    true);
                         }
                     });
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
    m_HotkeyScreenshotHistory = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("SCREENSHOT_HISTORY")), true, this);
    QObject::connect(m_HotkeyScreenshotHistory,
                     &QHotkey::activated,
                     qApp,
                     [this]() { history(); });
#endif
}

Flameshot* Flameshot::instance()
{
    static Flameshot c;
    return &c;
}

CaptureWidget* Flameshot::gui(const CaptureRequest& req)
{
    if (!resolveAnyConfigErrors()) {
        return nullptr;
    }

    CaptureRequest request = req;
    if (request.captureMode() == CaptureRequest::GRAPHICAL_MODE &&
        request.initialSelection().isNull() &&
        ConfigHandler().saveLastRegion()) {
        request.setInitialSelection(getLastRegion());
    }

    // The overlay asked for another monitor (Tab) before closing itself.
    if (!request.hasSelectedMonitor() && m_pendingMonitor >= 0) {
        request.setSelectedMonitor(m_pendingMonitor);
        m_pendingMonitor = -1;
    }

    // An overlay that was hidden on purpose (monitor switch) is still alive
    // until deleteLater runs; drop it so the replacement can be created.
    if (!m_captureWindow.isNull() && !m_captureWindow->isVisible()) {
        m_captureWindow->deleteLater();
        m_captureWindow = nullptr;
    }

#if defined(Q_OS_MACOS)
    // This is required on MacOS because of Mission Control. If you'll switch to
    // another Desktop you cannot take a new screenshot from the tray, you have
    // to switch back to the Flameshot Desktop manually. It is not obvious and a
    // large number of users are confused and report a bug.
    if (m_captureWindow != nullptr) {
        m_captureWindow->close();
        delete m_captureWindow;
        m_captureWindow = nullptr;
    }
#endif

    if (nullptr == m_captureWindow) {
        // TODO is this unnecessary now?
        int timeout = 5000; // 5 seconds
        const int delay = 100;
        QWidget* modalWidget = nullptr;
        for (; timeout >= 0; timeout -= delay) {
            modalWidget = qApp->activeModalWidget();
            if (nullptr == modalWidget) {
                break;
            }
            modalWidget->close();
            modalWidget->deleteLater();
            QThread::msleep(delay);
        }
        if (0 == timeout) {
            QMessageBox::warning(
              nullptr, tr("Error"), tr("Unable to close active modal widgets"));
            return nullptr;
        }

        m_captureWindow = new CaptureWidget(request);

#ifdef Q_OS_WIN
        m_captureWindow->show();
        // A hotkey launched from the tray daemon does not own the foreground,
        // so Windows would leave the overlay unfocused and drop Esc / Ctrl+C.
        FlameshotWindow::forceForeground(m_captureWindow);
#elif defined(Q_OS_MACOS)
        if (ConfigHandler().useNativeFullscreen()) {
            m_captureWindow->showFullScreen();
        } else {
            m_captureWindow->show();
            raiseAboveMenuBar(m_captureWindow);
        }
        m_captureWindow->activateWindow();
        m_captureWindow->raise();
#else
        m_captureWindow->showFullScreen();
//        m_captureWindow->show(); // For CaptureWidget Debugging under Linux
#endif
        return m_captureWindow;
    } else {
        // An overlay is already open (typically the user pressed the hotkey
        // twice): surface it instead of silently doing nothing.
        raiseCaptureWindow();
        emit captureFailed();
        return nullptr;
    }
}

void Flameshot::screen(CaptureRequest req, const int screenNumber)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = false;
    QPixmap p;
    QRect geometry;

    if (screenNumber < 0) {
        ScreenGrabber grabber;
        p = grabber.grabEntireDesktop(ok);
        if (ok) {
            QScreen* selectedScreen = grabber.getSelectedScreen();
            if (selectedScreen) {
                geometry = ScreenGrabber().screenGeometry(selectedScreen);
            } else {
                ok = false;
            }
        }
    } else if (screenNumber >= qApp->screens().count()) {
        AbstractLogger() << QObject::tr(
          "Requested screen exceeds screen count");
        ok = false;
    } else {
        // Specific screen number provided - use grabScreen to bypass selector
        QScreen* screen = qApp->screens()[screenNumber];
        p = ScreenGrabber().grabScreen(screen, ok);
        if (ok) {
            geometry = ScreenGrabber().screenGeometry(screen);
        }
    }

    if (ok) {
        QRect region = req.initialSelection();
        if (region.isNull()) {
            region = geometry;
        } else {
            QRect screenGeom = geometry;
            screenGeom.moveTopLeft({ 0, 0 });
            region = region.intersected(screenGeom);
            p = p.copy(region);
        }
        if (req.tasks() & CaptureRequest::PIN) {
            // change geometry for pin task
            req.addPinTask(region);
        }
        exportCapture(p, geometry, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::full(const CaptureRequest& req)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = true;
    QPixmap p(ScreenGrabber().grabFullDesktop(ok));
    if (ok) {
        QRect selection; // `flameshot full` does not support region selection
        exportCapture(p, selection, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::launcher()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_launcherWindow == nullptr) {
        m_launcherWindow = new CaptureLauncher();
    }
    m_launcherWindow->show();
#if defined(Q_OS_MACOS)
    showDockIcon(m_launcherWindow);
#endif
}

void Flameshot::onLanguageChanged()
{
    // Hand-built Qt widgets do not re-run their constructor-time setText()
    // calls on QEvent::LanguageChange, so re-create the config window in
    // place. QPointer makes the old pointer safe while deleteLater() runs.
    if (m_configWindow) {
        const int tab = m_configWindow->currentTab();
        const QPoint pos = m_configWindow->pos();
        m_configWindow->deleteLater();
        m_configWindow = nullptr;
        config();
        if (m_configWindow) {
            m_configWindow->setCurrentTab(tab);
            m_configWindow->move(pos);
        }
    }
    // Short-lived windows: close so the next open uses the new language.
    if (m_infoWindow) {
        m_infoWindow->deleteLater();
        m_infoWindow = nullptr;
    }
    if (m_launcherWindow) {
        m_launcherWindow->deleteLater();
        m_launcherWindow = nullptr;
    }
}

void Flameshot::config()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_configWindow == nullptr) {
        m_configWindow = new ConfigWindow();
        m_configWindow->show();
        // Call show() first, otherwise the correct geometry cannot be fetched
        // for centering the window on the screen
        QRect position = m_configWindow->frameGeometry();
        QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
        position.moveCenter(currentScreen->availableGeometry().center());
        m_configWindow->move(position.topLeft());
#if defined(Q_OS_MACOS)
        showDockIcon(m_configWindow);
#endif
    }
}

void Flameshot::info()
{
    if (m_infoWindow == nullptr) {
        m_infoWindow = new InfoWindow();
#if defined(Q_OS_MACOS)
        showDockIcon(m_infoWindow);
#endif
    }
}

#ifdef ENABLE_IMGUR
void Flameshot::history()
{
    static UploadHistory* historyWidget = nullptr;
    if (historyWidget == nullptr) {
        historyWidget = new UploadHistory;
        historyWidget->loadHistory();
        connect(historyWidget, &QObject::destroyed, this, []() {
            historyWidget = nullptr;
        });
    }

    historyWidget->show();
    // Call show() first, otherwise the correct geometry cannot be fetched
    // for centering the window on the screen
    QRect position = historyWidget->frameGeometry();
    QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
    position.moveCenter(currentScreen->availableGeometry().center());
    historyWidget->move(position.topLeft());

#if defined(Q_OS_MACOS)
    showDockIcon(historyWidget);
#endif
}
#endif

#if defined(Q_OS_MACOS)
void Flameshot::onWindowVisibilityChanged(QWindow::Visibility newVisibility)
{
    auto* qw = qobject_cast<QWindow*>(sender());
    if (!qw) {
        return;
    }

    if (newVisibility == QWindow::Hidden) {
        qw->setProperty(visibleInDockProperty, false);
        --m_dockIconVisibleCount;
        if (m_dockIconVisibleCount == 0) {
            setActivationPolicyAccessory();
        }
    } else {
        bool windowTrackedInDock = qw->property(visibleInDockProperty).toBool();
        if (!windowTrackedInDock) {
            qw->setProperty(visibleInDockProperty, true);
            ++m_dockIconVisibleCount;
            setActivationPolicyRegular();
        }
    }
}

void Flameshot::showDockIcon(QWidget* w)
{
    QWindow* qw = w->windowHandle();
    if (!qw) {
        return;
    }

    connect(qw,
            &QWindow::visibilityChanged,
            this,
            &Flameshot::onWindowVisibilityChanged);
}
#endif

void Flameshot::openSavePath()
{
    QString savePath = ConfigHandler().savePath();
    if (!savePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    }
}

QVersionNumber Flameshot::getVersion()
{
    return QVersionNumber::fromString(
      QStringLiteral(APP_VERSION).replace("v", ""));
}

/**
 * @brief Is a capture overlay already on screen in this process?
 *
 * The tray daemon answers hotkeys itself, so a forwarded "capture" request
 * must not stack a second overlay on top of the first one.
 */
bool Flameshot::hasCaptureWindow() const
{
    return !m_captureWindow.isNull();
}

void Flameshot::raiseCaptureWindow()
{
    if (m_captureWindow.isNull()) {
        return;
    }
    m_captureWindow->show();
    m_captureWindow->raise();
#if defined(Q_OS_WIN)
    FlameshotWindow::forceForeground(m_captureWindow);
#else
    m_captureWindow->activateWindow();
#endif
}

/**
 * @brief Re-open the region selector on another monitor.
 *
 * The old overlay hides itself immediately and calls this; the short delay
 * lets its deferred deletion finish before the replacement is built, so two
 * full-screen windows never coexist.
 */
void Flameshot::requestCaptureOnMonitor(int monitorIndex)
{
    m_pendingMonitor = monitorIndex;
    QTimer::singleShot(80, this, [this]() {
        requestCapture(CaptureRequest(CaptureRequest::GRAPHICAL_MODE));
    });
}

void Flameshot::requestScrollCapture(const QRect& region)
{
    // The overlay that triggered this is still on screen (it closes at the end
    // of the same call), and it is a frozen picture anyway: the scrolling has
    // to happen on the live desktop. Give the window a moment to disappear,
    // then take over. Doing it from a timer also keeps the scrolling session
    // out of the overlay's own event handler, which is about to be destroyed.
    m_pendingScrollRegion = region;
    QTimer::singleShot(150, this, [this]() {
        const QRect pending = m_pendingScrollRegion;
        m_pendingScrollRegion = QRect();
        ScrollCapture::instance()->start(pending);
    });
}

void Flameshot::setOrigin(Origin origin)
{
    m_origin = origin;
}

Flameshot::Origin Flameshot::origin()
{
    return m_origin;
}

/**
 * @brief Prompt the user to resolve config errors if necessary.
 * @return Whether errors were resolved.
 */
bool Flameshot::resolveAnyConfigErrors()
{
    bool resolved = true;
    ConfigHandler confighandler;
    if (!confighandler.checkUnrecognizedSettings() ||
        !confighandler.checkSemantics()) {
        auto* resolver = new ConfigResolver();
        QObject::connect(
          resolver, &ConfigResolver::rejected, [resolver, &resolved]() {
              resolved = false;
              resolver->deleteLater();
              if (origin() == CLI) {
                  exit(1);
              }
          });
        QObject::connect(
          resolver, &ConfigResolver::accepted, [resolver, &resolved]() {
              resolved = true;
              resolver->close();
              resolver->deleteLater();
              // Ensure that the dialog is closed before starting capture
              qApp->processEvents();
          });
        resolver->exec();
        qApp->processEvents();
    }
    return resolved;
}

void Flameshot::requestCapture(const CaptureRequest& request)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    switch (request.captureMode()) {
        case CaptureRequest::FULLSCREEN_MODE:
            QTimer::singleShot(request.delay(),
                               [this, request] { full(request); });
            break;
        case CaptureRequest::SCREEN_MODE: {
            int&& number = request.data().toInt();
            QTimer::singleShot(request.delay(), [this, request, number]() {
                screen(request, number);
            });
            break;
        }
        case CaptureRequest::GRAPHICAL_MODE: {
            QTimer::singleShot(
              request.delay(), this, [this, request]() { gui(request); });
            break;
        }
        default:
            emit captureFailed();
            break;
    }
}

void Flameshot::exportCapture(const QPixmap& capture,
                              QRect& selection,
                              const CaptureRequest& req)
{
    using CR = CaptureRequest;
    int tasks = req.tasks(), mode = req.captureMode();
    QString path = req.path();

    if (tasks & CR::PRINT_GEOMETRY) {
        QTextStream(stdout)
          << selection.width() << "x" << selection.height() << "+"
          << selection.x() << "+" << selection.y() << "\n";
    }

    if (tasks & CR::PRINT_RAW) {
        QByteArray byteArray;
        QBuffer buffer(&byteArray);
        capture.save(&buffer, "PNG");
        if (QFile file; file.open(stdout, QIODevice::WriteOnly)) {
            file.write(byteArray);
            file.close();
        }
    }

    if (tasks & CR::SAVE) {
        if (req.path().isEmpty()) {
            saveToFilesystemGUI(capture);
        } else {
            saveToFilesystem(capture, path);
        }
    }

    if (tasks & CR::COPY) {
        FlameshotDaemon::copyToClipboard(capture);
    }

    if (tasks & CR::PIN) {
        FlameshotDaemon::createPin(capture, selection);
        if (mode == CR::SCREEN_MODE || mode == CR::FULLSCREEN_MODE) {
            AbstractLogger::info()
              << QObject::tr("Full screen screenshot pinned to screen");
        }
    }

#ifdef ENABLE_IMGUR
    if (tasks & CR::UPLOAD) {
        if (!ConfigHandler().uploadWithoutConfirmation()) {
            auto* dialog = new ImgUploadDialog();
            if (dialog->exec() == QDialog::Rejected) {
                return;
            }
        }

        ImgUploaderBase* widget = ImgUploaderManager().uploader(capture);
        widget->show();
        widget->activateWindow();
        // NOTE: lambda can't capture 'this' because it might be destroyed later
        CR::ExportTask tasks = tasks;
        QObject::connect(
          widget, &ImgUploaderBase::uploadOk, [=, this](const QUrl& url) {
              if (ConfigHandler().copyURLAfterUpload()) {
                  if (!(tasks & CR::COPY)) {
                      FlameshotDaemon::copyToClipboard(
                        url.toString(), tr("URL copied to clipboard."));
                  }
                  widget->showPostUploadDialog();
              }
          });
    }
#endif

    if (!(tasks & CR::UPLOAD)) {
        emit captureTaken(capture);
    }
}

void Flameshot::setExternalWidget(bool b)
{
    m_haveExternalWidget = b;
}
bool Flameshot::haveExternalWidget()
{
    return m_haveExternalWidget;
}

// STATIC ATTRIBUTES
Flameshot::Origin Flameshot::m_origin = Flameshot::DAEMON;
