// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#ifdef USE_KDSINGLEAPPLICATION
#include <kdsingleapplication.h>
#ifdef Q_OS_UNIX
#include "core/signaldaemon.h"
#include <csignal>
#endif
#endif

#include "cli/commandlineparser.h"
#include "config/cacheutils.h"
#include "config/styleoverride.h"
#include "core/capturerequest.h"
#include "core/flameshot.h"
#include "core/flameshotdaemon.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"
#include "utils/crashhandler.h"
#include "utils/filenamehandler.h"
#include "utils/pathinfo.h"
#include "utils/valuehandler.h"

#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
#include "core/flameshotdbusadapter.h"
#include <QDBusConnection>
#include <QDBusMessage>
#endif

#if defined(Q_OS_WIN)
#include "utils/scrollcapture.h"
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

#include <QDir>
#include <QFile>
#include <QTextStream>

#ifdef ENABLE_SELFTEST
#include "tools/capturecontext.h"
#include "tools/circlecount/circlecounttool.h"
#include "utils/scrollcapture.h"
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QTextStream>
#endif

#include <QApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QNetworkProxyFactory>
#include <QSharedMemory>
#include <QTimer>
#include <QTranslator>
#include "utils/translationmanager.h"
#include "utils/zaggertr.h"

// Required for saving button list QList<CaptureTool::Type>
Q_DECLARE_METATYPE(QList<int>)

#if defined(USE_KDSINGLEAPPLICATION) && defined(Q_OS_UNIX)
static int setup_unix_signal_handlers()
{
    struct sigaction sint, term;

    sint.sa_handler = SignalDaemon::intSignalHandler;
    sigemptyset(&sint.sa_mask);
    sint.sa_flags = 0;
    sint.sa_flags |= SA_RESTART;

    if (sigaction(SIGINT, &sint, 0))
        return 1;

    term.sa_handler = SignalDaemon::termSignalHandler;
    sigemptyset(&term.sa_mask);
    term.sa_flags = 0;
    term.sa_flags |= SA_RESTART;

    if (sigaction(SIGTERM, &term, 0))
        return 2;

    return 0;
}
#endif

#if defined(Q_OS_WIN)
/**
 * @brief `flameshot doctor` - answer "is it running, and did it crash?".
 *
 * Reads public state only: the process list, the config, the tail of the log
 * and the crash reports. It never scrolls, grabs pixels or takes focus, so it
 * is safe to run while the desktop is in use - which is exactly when somebody
 * needs to know why the tray icon is gone.
 */
int runDoctor()
{
    QTextStream out(stdout);

    // 1. is a daemon/capture process alive?
    QList<unsigned long> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"flameshot.exe") == 0 &&
                    entry.th32ProcessID != GetCurrentProcessId()) {
                    pids << entry.th32ProcessID;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    out << ZAG_TR("=== Flameshot self-test ===") << "\n";
    out << ZAG_TR("Processes:") << " " << pids.size() << "\n";
    for (unsigned long pid : pids) {
        out << "  pid " << pid << "\n";
    }

    // 2. would the tray show up at all?
    ConfigHandler config;
    out << ZAG_TR("Config:") << " " << config.configFilePath() << "\n";
    out << "  disabledTrayIcon   = "
        << (config.disabledTrayIcon() ? "true" : "false") << "\n";
    out << "  autoCloseIdleDaemon= "
        << (config.autoCloseIdleDaemon() ? "true" : "false") << "\n";
    out << "  TAKE_SCREENSHOT    = " << config.shortcut("TAKE_SCREENSHOT")
        << "\n";
    out << "  SCROLL_CAPTURE     = " << config.shortcut("SCROLL_CAPTURE")
        << "\n";
    out << "  TYPE_CIRCLECOUNT   = " << config.shortcut("TYPE_CIRCLECOUNT")
        << "\n";

    // 3. did it crash since the last time somebody asked?
    const QString logDir = CrashHandler::logDirectory();
    const QString crashDir = logDir + QStringLiteral("/crashes");
    const QStringList crashes =
      QDir(crashDir).entryList({ "crash-*", "abort-*" }, QDir::Files);
    out << ZAG_TR("Log:") << " " << CrashHandler::logFilePath() << "\n";
    out << ZAG_TR("Crash reports:") << " " << crashes.size();
    if (crashes.isEmpty()) {
        out << ZAG_TR("(no crash records: the process exited normally or was terminated, not a crash)");
    }
    out << "\n";
    for (const QString& name : crashes) {
        out << "  " << crashDir << "/" << name << "\n";
    }

    // 4. what happened last?
    QFile log(CrashHandler::logFilePath());
    if (log.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(log.readAll())
                                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const int from = qMax(0, lines.size() - 12);
        out << ZAG_TR("--- Last %1 log lines ---").arg(lines.size() - from) << "\n";
        for (int i = from; i < lines.size(); ++i) {
            out << lines.at(i) << "\n";
        }
    }
    out.flush();
    return 0;
}
#endif

int requestCaptureAndWait(const CaptureRequest& req){
    Flameshot* flameshot = Flameshot::instance();
    flameshot->requestCapture(req);
    QObject::connect(flameshot, &Flameshot::captureTaken, [&](const QPixmap&) {
#if defined(Q_OS_MACOS)
        // Only useful on MacOS because each instance hosts its own widgets
        if (!FlameshotDaemon::isThisInstanceHostingWidgets()) {
            qApp->exit(0);
        }
#else
        // if this instance is not daemon, make sure it exit after caputre finish
        if (FlameshotDaemon::instance() == nullptr && !Flameshot::instance()->haveExternalWidget()) {
            qApp->exit(E_OK);
        }
#endif
    });
    QObject::connect(flameshot, &Flameshot::captureFailed, []() {
        AbstractLogger::Target logTarget = static_cast<AbstractLogger::Target>(
          ConfigHandler().showAbortNotification()
            ? AbstractLogger::Target::Default
            : AbstractLogger::Target::Default &
                ~AbstractLogger::Target::Notification);
        AbstractLogger::info(logTarget) << "Screenshot aborted.";
        qApp->exit(E_ABORTED);
    });
    const int exitCode = qApp->exec();
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("process exit: pid=%1 code=%2 (capture)")
           .arg(QCoreApplication::applicationPid())
           .arg(exitCode);
    return exitCode;
}QSharedMemory* guiMutexLock()
{
    QString key = "org.flameshot.Flameshot-" APP_VERSION;
    auto* shm = new QSharedMemory(key);
#ifdef Q_OS_UNIX
    // Destroy shared memory if the last instance crashed on Unix
    shm->attach();
    delete shm;
    shm = new QSharedMemory(key);
#endif
    if (!shm->create(1)) {
        delete shm;
        return nullptr;
    }
    return shm;
}

void configureApp(bool gui)
{
    if (gui) {
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        QApplication::setStyle("Fusion"); // Supports dark scheme on Win 10/11
#else
        QApplication::setStyle(new StyleOverride);
#endif
    }

    auto app = QCoreApplication::instance();
    app->setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
    // TranslationManager owns the translators for the whole process, so the
    // UI language can be switched at runtime without restarting the daemon.
    TranslationManager::instance()->initialize();
}

#if defined(USE_KDSINGLEAPPLICATION)
namespace {

/**
 * @brief One flameshot per desktop session.
 *
 * Before this, only the "no arguments" path created a KDSingleApplication
 * object. `flameshot daemon` (the autostart entry) did not, so the daemon was
 * invisible to that check: every later launch became "primary" and started a
 * second daemon with a second tray icon. A few launches produced the pile of
 * tray icons users reported, and the duplicates kept a grab on the hotkeys.
 *
 * Now every long lived mode goes through this door. The first process stays
 * the single owner of the tray and the hotkeys; the others hand their verb
 * over to it and exit.
 */
KDSingleApplication* g_singleInstance = nullptr;

bool claimSingleInstance(const QString& verb)
{
    if (g_singleInstance == nullptr) {
        g_singleInstance =
          new KDSingleApplication(QStringLiteral("org.flameshot.Flameshot"));
    }
    if (g_singleInstance->isPrimaryInstance()) {
        return true;
    }
#if defined(Q_OS_WIN)
    // Tell the log *who* owns the tray, so "the tray disappeared / there are
    // several of them" reports have a primary source of truth.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"flameshot.exe") == 0 &&
                    entry.th32ProcessID != GetCurrentProcessId()) {
                    AbstractLogger::info(AbstractLogger::LogFile)
                      << QStringLiteral(
                           "another flameshot instance is alive (pid=%1), "
                           "forwarding verb '%2' and exiting")
                           .arg(entry.th32ProcessID)
                           .arg(verb);
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#endif
    // Empty verb means "just make sure no second tray appears".
    if (!verb.isEmpty()) {
        g_singleInstance->sendMessage(verb.toUtf8());
    }
    return false;
}

} // namespace
#endif

/**
 * @brief Route verbs forwarded by a second process into the primary one.
 *
 * Runs in the primary process only. A forwarded verb never starts a second
 * copy of the UI: it reuses the overlay that is already on screen instead.
 */
void dispatchForwardedRequest(const QByteArray& raw);

// The single-instance channel carries two different kinds of traffic: plain
// verbs from a second invocation of our own command line ("capture",
// "config", ...) and QDataStream payloads from the pin/clipboard helpers.
// Route them by shape once, so the daemon handler stops warning about verbs
// it was never meant to understand.
bool isForwardedVerb(const QByteArray& message)
{
    return message == QByteArrayLiteral("capture") ||
           message == QByteArrayLiteral("scroll") ||
           message == QByteArrayLiteral("config") ||
           message == QByteArrayLiteral("launcher") ||
           message == QByteArrayLiteral("quit") ||
           message == QByteArrayLiteral("daemon");
}

void routeMessage(const QByteArray& message)
{
    if (isForwardedVerb(message)) {
        dispatchForwardedRequest(message);
        return;
    }
    if (FlameshotDaemon::instance() != nullptr) {
        FlameshotDaemon::instance()->messageReceivedFromSecondaryInstance(
          message);
    }
}

void dispatchForwardedRequest(const QByteArray& raw)
{
    const QString verb = QString::fromUtf8(raw);
    AbstractLogger::info() << QStringLiteral(
                                "Second instance forwarded '%1'")
                                .arg(verb);

    if (verb == QLatin1String("capture")) {
        Flameshot* flameshot = Flameshot::instance();
        if (flameshot->hasCaptureWindow()) {
            flameshot->raiseCaptureWindow();
            return;
        }
        flameshot->requestCapture(
          CaptureRequest(CaptureRequest::GRAPHICAL_MODE));
    } else if (verb == QLatin1String("scroll")) {
#if defined(Q_OS_WIN)
        ScrollCapture::instance()->start();
#endif
    } else if (verb == QLatin1String("config")) {
        Flameshot::instance()->config();
    } else if (verb == QLatin1String("launcher")) {
        Flameshot::instance()->launcher();
    } else if (verb == QLatin1String("quit")) {
        // Graceful shutdown. A killed daemon leaves its notification icon
        // behind as a dead entry that Explorer keeps showing until the user
        // hovers it; the installer used to TerminateProcess every upgrade, so
        // each update added one more ghost icon ("托盘有六个"). Hiding the
        // tray icon first sends NIM_DELETE and the shell forgets it.
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Graceful shutdown requested by a second instance");
        if (FlameshotDaemon::instance() != nullptr) {
            FlameshotDaemon::instance()->shutdownForExit();
        } else {
            QTimer::singleShot(0, qApp, []() { QCoreApplication::quit(); });
        }
    } else if (verb == QLatin1String("daemon")) {
        // Autostart raced with an already running instance. Never answer with
        // silence: a user double-clicking the shortcut must get visible
        // feedback, otherwise "nothing happens" reads as "the app is broken".
        if (FlameshotDaemon::instance() != nullptr) {
            FlameshotDaemon::instance()->sendTrayNotification(
              ZAG_TR("Press Ctrl+Alt+A to capture, Ctrl+Alt+S for scrolling capture"),
              ZAG_TR("Flameshot is already running"));
        }
    } else {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("Unknown forwarded request: %1").arg(verb);
    }
}

#ifdef ENABLE_SELFTEST
// Acceptance harness: render markers with the *production* CircleCountTool
// and measure the resulting pixels. Not a mock and not a formula replay.
static int runRenderSelfTest(const QString& outputDir)
{
    QDir().mkpath(outputDir);
    QTextStream out(stdout);

    for (int toolSize : { 1, 4, 5, 15, 50 }) {
        QPixmap pixmap(240, 240);
        pixmap.fill(Qt::white);

        CaptureContext ctx;
        ctx.color = QColor(255, 0, 0);
        ctx.mousePos = QPoint(120, 120);
        ctx.toolSize = toolSize;
        ctx.circleCount = 1;

        CircleCountTool tool;
        tool.drawStart(ctx);
        QPainter painter(&pixmap);
        tool.process(painter, pixmap);
        painter.end();

        const QString file = QDir(outputDir).filePath(
          QStringLiteral("marker-%1.png").arg(toolSize));
        const bool saved = pixmap.save(file, "PNG");

        const QImage img =
          pixmap.toImage().convertToFormat(QImage::Format_RGB32);
        int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
        bool contrastPixels = false;
        for (int y = 0; y < img.height(); ++y) {
            const auto* row =
              reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb p = row[x];
                // Ignore near-white background and antialiasing fringe.
                if (qRed(p) > 215 && qGreen(p) > 215 && qBlue(p) > 215) {
                    continue;
                }
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
                // Anything not the marker colour is a digit / outline.
                if (!(qRed(p) > 180 && qGreen(p) < 80 && qBlue(p) < 80)) {
                    contrastPixels = true;
                }
            }
        }

        out << "size=" << toolSize << " bbox="
            << (maxX - minX + 1) << "x" << (maxY - minY + 1)
            << " digit_or_outline=" << (contrastPixels ? "yes" : "no")
            << " png=" << file << " saved=" << (saved ? "yes" : "no")
            << "\n";
        if (toolSize == 1) {
            for (int y = 115; y <= 124; ++y) {
                QString line;
                for (int x = 115; x <= 124; ++x) {
                    const QRgb p = img.pixel(x, y);
                    const int diff = 255 - qMin(qRed(p), qGreen(p));
                    line += diff > 128 ? '#' : (diff > 8 ? '+' : '.');
                }
                out << "    " << line << "\n";
            }
        }
    }
    // Wheel semantics of the counter tool, exercised on the production class:
    // a plain wheel is not consumed so CaptureWidget::setToolSize() resizes the
    // bubble; Ctrl+wheel consumes it and steps the next number instead.
    {
        CaptureContext plainCtx;
        plainCtx.circleCount = 7;
        CircleCountTool plainTool;
        const bool plainConsumed =
          plainTool.handleMouseWheelEvent(1, false, plainCtx);

        CaptureContext ctrlCtx;
        ctrlCtx.circleCount = 7;
        CircleCountTool ctrlTool;
        const bool ctrlConsumed =
          ctrlTool.handleMouseWheelEvent(1, true, ctrlCtx);

        out << "wheel plain consumed=" << (plainConsumed ? "yes" : "no")
            << " (expect no -> resizes marker)"
            << " ctrl consumed=" << (ctrlConsumed ? "yes" : "no")
            << " (expect yes -> number 7->" << ctrlCtx.circleCount << ")\n";
    }

    // ScrollRegionPicker selftest: synthetic background, keyboard-driven
    // move / zoom / confirm / cancel on an off-screen widget. No real
    // desktop pixels are touched.
#if defined(Q_OS_WIN)
    {
        QString pickerReport;
        const bool pickerOk = ScrollCapture::pickerSelfTest(pickerReport);
        out << "picker " << (pickerOk ? "PASS" : "FAIL") << " "
            << pickerReport << "\n";
        if (!pickerOk) {
            out.flush();
            return 1;
        }
    }
#endif

    out.flush();
    return 0;
}
#endif

int main(int argc, char* argv[])
{

    // Required for saving button list QList<CaptureTool::Type>
    qRegisterMetaType<QList<int>>();

    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setApplicationName(QStringLiteral("flameshot"));
    QCoreApplication::setOrganizationName(QStringLiteral("flameshot"));
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    // One QApplication for the whole process. The previous code created a
    // QCoreApplication and then deleted it to build a QApplication in place
    // ("reinitializeAsQApplication"). Tearing the application object down
    // while Qt's platform integration, screen list and static caches still
    // reference it is undefined behaviour, and it left the process without a
    // platform plugin whenever a widget was constructed a moment too early.
    QApplication app(argc, argv);
    configureApp(true);
    CrashHandler::install();

    // One line per launch in the rolling log. Together with the matching
    // "process exit" line it answers the only question that matters when the
    // tray icon is gone: did the process stop on its own, and with which verb?
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("process start: pid=%1 verb=%2 args=%3")
           .arg(QCoreApplication::applicationPid())
           .arg(argc > 1 ? QString::fromLocal8Bit(argv[1])
                         : QStringLiteral("(none)"))
           .arg(QCoreApplication::arguments().mid(1).join(QLatin1Char(' ')));

    // no arguments, just launch Flameshot
    if (argc == 1) {
#ifdef USE_KDSINGLEAPPLICATION
#ifdef Q_OS_UNIX
        setup_unix_signal_handlers();
        auto signalDaemon = SignalDaemon();
#endif
        // A second launch must not add a second tray icon; it asks the
        // running instance for a capture instead.
        if (!claimSingleInstance(QStringLiteral("capture")) &&
            !ConfigHandler().allowMultipleGuiInstances()) {
            return 0;
        }
#endif

        auto c = Flameshot::instance();
        FlameshotDaemon::start();

#if defined(USE_KDSINGLEAPPLICATION)
        if (g_singleInstance != nullptr &&
            g_singleInstance->isPrimaryInstance()) {
            QObject::connect(
              g_singleInstance,
              &KDSingleApplication::messageReceived,
              [](const QByteArray& message) { routeMessage(message); });
        }
#endif

#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
        new FlameshotDBusAdapter(c);
        QDBusConnection dbus = QDBusConnection::sessionBus();
        if (!dbus.isConnected()) {
            AbstractLogger::error()
              << QObject::tr("Unable to connect via DBus");
        }
        dbus.registerObject(QStringLiteral("/"), c);
        dbus.registerService(QStringLiteral("org.flameshot.Flameshot"));
#endif
        const int exitCode = qApp->exec();
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("process exit: pid=%1 code=%2 (no arguments)")
               .arg(QCoreApplication::applicationPid())
               .arg(exitCode);
        return exitCode;
    }

    /*--------------|
     * CLI parsing  |
     * ------------*/
    CommandLineParser parser;
    // Add description
    parser.setDescription(
      QObject::tr("Powerful yet simple to use screenshot software."));
    parser.setGeneralErrorMessage(QObject::tr("See") + " flameshot --help.");
    // Arguments
    CommandArgument fullArgument(
      QStringLiteral("full"),
      QObject::tr("Capture screenshot of all monitors at the same time."));
    CommandArgument launcherArgument(QStringLiteral("launcher"),
                                     QObject::tr("Open the capture launcher."));
    CommandArgument daemonArgument(
      QStringLiteral("daemon"),
      QObject::tr("Run in the background: tray icon and global hotkeys only."));
    CommandArgument quitArgument(
      QStringLiteral("quit"),
      QObject::tr("Ask the running instance to shut down and remove its tray "
                  "icon. Never force-kill the daemon: a killed process leaves "
                  "a dead tray icon behind."));
    CommandArgument guiArgument(
      QStringLiteral("gui"),
      QObject::tr("Start a manual capture in GUI mode."));
    CommandArgument configArgument(QStringLiteral("config"),
                                   QObject::tr("Configure") + " flameshot.");
    CommandArgument screenArgument(
      QStringLiteral("screen"),
      QObject::tr("Capture a screenshot of the specified monitor."));
#ifdef Q_OS_WIN
    CommandArgument scrollArgument(
      QStringLiteral("scroll"),
      QObject::tr("Capture a scrolling screenshot of a selected region."));
    CommandArgument doctorArgument(
      QStringLiteral("doctor"),
      QObject::tr("Report processes, tray configuration, crash reports and the "
                  "tail of the log. Read only."));
#endif
#ifdef ENABLE_SELFTEST
    CommandArgument selftestArgument(
      QStringLiteral("selftest"),
      QObject::tr("Render annotation markers and measure their pixels."));
#endif

    // Options
    CommandOption pathOption(
      { "p", "path" },
      QObject::tr("Existing directory or new file to save to"),
      QStringLiteral("path"));
#ifdef Q_OS_WIN
    CommandOption probeOption(
      QStringLiteral("probe"),
      QObject::tr("Only report which window and scroll container would be "
                  "used, without scrolling anything."));
#endif
    CommandOption clipboardOption(
      { "c", "clipboard" }, QObject::tr("Save the capture to the clipboard"));
    CommandOption pinOption("pin",
                            QObject::tr("Pin the capture to the screen"));
    CommandOption delayOption({ "d", "delay" },
                              QObject::tr("Delay time in milliseconds"),
                              QStringLiteral("milliseconds"));

    CommandOption useLastRegionOption(
      "last-region",
      QObject::tr("Repeat screenshot with previously selected region"));

    CommandOption regionOption("region",
                               QObject::tr("Screenshot region to select"),
                               QStringLiteral("WxH+X+Y or string"));
    CommandOption filenameOption({ "f", "filename" },
                                 QObject::tr("Set the filename pattern"),
                                 QStringLiteral("pattern"));
    CommandOption acceptOnSelectOption(
      { "s", "accept-on-select" },
      QObject::tr("Accept capture as soon as a selection is made"));
    CommandOption trayOption({ "t", "trayicon" },
                             QObject::tr("Enable or disable the trayicon"),
                             QStringLiteral("bool"));
    CommandOption autostartOption(
      { "a", "autostart" },
      QObject::tr("Enable or disable run at startup"),
      QStringLiteral("bool"));
    CommandOption notificationOption(
      { "n", "notifications" },
      QObject::tr("Enable or disable the notifications"),
      QStringLiteral("bool"));
    CommandOption checkOption(
      "check", QObject::tr("Check the configuration for errors"));
    CommandOption showHelpOption(
      { "s", "showhelp" },
      QObject::tr("Show the help message in the capture mode"),
      QStringLiteral("bool"));
    CommandOption mainColorOption({ "m", "maincolor" },
                                  QObject::tr("Define the main UI color"),
                                  QStringLiteral("color-code"));
    CommandOption contrastColorOption(
      { "k", "contrastcolor" },
      QObject::tr("Define the contrast UI color"),
      QStringLiteral("color-code"));
    CommandOption rawImageOption({ "r", "raw" },
                                 QObject::tr("Print raw PNG capture"));
    CommandOption selectionOption(
      { "g", "print-geometry" },
      QObject::tr("Print geometry of the selection in the format WxH+X+Y. Does "
                  "nothing if raw is specified"));
    CommandOption screenNumberOption(
      { "n", "number" },
      QObject::tr("Define the screen to capture (starting from 0)") + ",\n" +
        QObject::tr("default: screen containing the cursor"),
      QObject::tr("Screen number"),
      QStringLiteral("-1"));
    CommandOption editOption(
      { "e", "edit" },
      QObject::tr("Interactively select and edit the screenshot region"));

    // Add checkers
    auto colorChecker = [](const QString& colorCode) -> bool {
        QColor parsedColor(colorCode);
        return parsedColor.isValid() && parsedColor.alphaF() == 1.0;
    };
    QString colorErr =
      QObject::tr("Invalid color, "
                  "this flag supports the following formats:\n"
                  "- #RGB (each of R, G, and B is a single hex digit)\n"
                  "- #RRGGBB\n- #RRRGGGBBB\n"
                  "- #RRRRGGGGBBBB\n"
                  "- Named colors like 'blue' or 'red'\n"
                  "You may need to escape the '#' sign as in '\\#FFF'");

    const QString delayErr =
      QObject::tr("Invalid delay, it must be a number greater than 0");
    const QString numberErr =
      QObject::tr("Invalid screen number, it must be non negative");
    const QString regionErr = QObject::tr(
      "Invalid region, use 'WxH+X+Y' or 'all' or 'screen0/screen1/...'.");
    auto numericChecker = [](const QString& delayValue) -> bool {
        bool ok;
        int value = delayValue.toInt(&ok);
        return ok && value >= 0;
    };
    auto regionChecker = [](const QString& region) -> bool {
        Region valueHandler;
        return valueHandler.check(region);
    };

    const QString pathErr =
      QObject::tr("Invalid path, must be an existing directory or a new file "
                  "in an existing directory");
    auto pathChecker = [pathErr](const QString& pathValue) -> bool {
        QFileInfo fileInfo(pathValue);
        if (fileInfo.isDir() || fileInfo.dir().exists()) {
            return true;
        } else {
            AbstractLogger::error() << QObject::tr(pathErr.toLatin1().data());
            return false;
        }
    };

    const QString booleanErr =
      QObject::tr("Invalid value, it must be defined as 'true' or 'false'");
    auto booleanChecker = [](const QString& value) -> bool {
        return value == QLatin1String("true") ||
               value == QLatin1String("false");
    };

    contrastColorOption.addChecker(colorChecker, colorErr);
    mainColorOption.addChecker(colorChecker, colorErr);
    delayOption.addChecker(numericChecker, delayErr);
    regionOption.addChecker(regionChecker, regionErr);
    useLastRegionOption.addChecker(booleanChecker, booleanErr);
    pathOption.addChecker(pathChecker, pathErr);
    trayOption.addChecker(booleanChecker, booleanErr);
    autostartOption.addChecker(booleanChecker, booleanErr);
    notificationOption.addChecker(booleanChecker, booleanErr);
    showHelpOption.addChecker(booleanChecker, booleanErr);
    screenNumberOption.addChecker(numericChecker, numberErr);

    // Relationships
    parser.AddArgument(guiArgument);
    parser.AddArgument(screenArgument);
    parser.AddArgument(fullArgument);
    parser.AddArgument(launcherArgument);
    parser.AddArgument(daemonArgument);
    parser.AddArgument(quitArgument);
    parser.AddArgument(configArgument);
#ifdef Q_OS_WIN
    parser.AddArgument(scrollArgument);
    parser.AddArgument(doctorArgument);
#endif
#ifdef ENABLE_SELFTEST
    parser.AddArgument(selftestArgument);
#endif
    auto helpOption = parser.addHelpOption();
    auto versionOption = parser.addVersionOption();
#ifdef ENABLE_SELFTEST
    parser.AddOptions({ pathOption }, selftestArgument);
#endif
#ifdef Q_OS_WIN
    parser.AddOptions({ probeOption }, scrollArgument);
#endif
    parser.AddOptions({ pathOption,
                        clipboardOption,
                        delayOption,
                        regionOption,
                        useLastRegionOption,
                        rawImageOption,
                        selectionOption,
                        pinOption,
                        acceptOnSelectOption },
                      guiArgument);
    parser.AddOptions({ screenNumberOption,
                        editOption,
                        clipboardOption,
                        pathOption,
                        delayOption,
                        regionOption,
                        rawImageOption,
                        pinOption },
                      screenArgument);
    parser.AddOptions(
      { pathOption, clipboardOption, delayOption, rawImageOption },
      fullArgument);
    parser.AddOptions({ autostartOption,
                        notificationOption,
                        filenameOption,
                        trayOption,
                        showHelpOption,
                        mainColorOption,
                        contrastColorOption,
                        checkOption },
                      configArgument);
    // Parse
    if (!parser.parse(qApp->arguments())) {
        goto finish;
    }

    // PROCESS DATA
    //--------------
    Flameshot::setOrigin(Flameshot::CLI);
#ifdef ENABLE_SELFTEST
    if (parser.isSet(selftestArgument)) {
        QString dir = parser.value(pathOption);
        if (dir.isEmpty()) {
            dir = QDir::currentPath();
        }
        runRenderSelfTest(dir);
        goto finish;
    }
#endif
#ifdef Q_OS_WIN
    if (parser.isSet(doctorArgument)) {
        runDoctor();
        goto finish;
    }
    if (parser.isSet(scrollArgument)) {
        if (parser.isSet(probeOption)) {
            ScrollCapture::diagnose();
        } else {
            ScrollCapture::instance()->start();
        }
        goto finish;
    }
#endif
    if (parser.isSet(helpOption) || parser.isSet(versionOption)) {
    } else if (parser.isSet(quitArgument)) { // QUIT
        // "flameshot quit" is the sanctioned way to stop the daemon: it asks
        // the running instance to hide its tray icon (NIM_DELETE) and exit,
        // so no dead icon is left in the notification area.
#if defined(USE_KDSINGLEAPPLICATION)
        if (!claimSingleInstance(QStringLiteral("quit"))) {
            // Verb forwarded to the primary instance; it will shut down.
            return 0;
        }
#endif
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("No running instance to stop");
        return 0;
    } else if (parser.isSet(daemonArgument)) { // DAEMON
        // Tray icon plus global hotkeys, nothing on screen. This is what the
        // autostart entry uses: `launcher` would flash a window on every boot.
#if defined(USE_KDSINGLEAPPLICATION)
        if (!claimSingleInstance(QStringLiteral("daemon"))) {
            return 0;
        }
        QObject::connect(g_singleInstance,
                         &KDSingleApplication::messageReceived,
                         [](const QByteArray& message) {
                             routeMessage(message);
                         });
#endif
        Flameshot::instance();
        FlameshotDaemon::start();
        if (FlameshotDaemon::instance() == nullptr) {
            return 1;
        }
        qApp->exec();
    } else if (parser.isSet(launcherArgument)) { // LAUNCHER
#if defined(USE_KDSINGLEAPPLICATION)
        if (!claimSingleInstance(QStringLiteral("launcher"))) {
            return 0;
        }
#endif
        Flameshot* flameshot = Flameshot::instance();
        flameshot->launcher();
        qApp->exec();
    } else if (parser.isSet(guiArgument)) { // GUI

        // Prevent multiple instances of 'flameshot gui' from running if not
        // configured to do so.
        if (!ConfigHandler().allowMultipleGuiInstances()) {
            auto* mutex = guiMutexLock();
            if (!mutex) {
                return 1;
            }
            QObject::connect(
              qApp, &QCoreApplication::aboutToQuit, qApp, [mutex]() {
                  mutex->detach();
                  delete mutex;
              });
        }

        // Option values
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        QString region = parser.value(regionOption);
        bool useLastRegion = parser.isSet(useLastRegionOption);
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);
        bool printGeometry = parser.isSet(selectionOption);
        bool pin = parser.isSet(pinOption);
        bool acceptOnSelect = parser.isSet(acceptOnSelectOption);
        CaptureRequest req(CaptureRequest::GRAPHICAL_MODE, delay, path);
        if (!region.isEmpty()) {
            auto selectionRegion = Region().value(region).toRect();
            req.setInitialSelection(selectionRegion);
        } else if (useLastRegion) {
            req.setInitialSelection(getLastRegion());
        }
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (printGeometry) {
            req.addTask(CaptureRequest::PRINT_GEOMETRY);
        }
        if (pin) {
            req.addTask(CaptureRequest::PIN);
        }
        if (acceptOnSelect) {
            req.addTask(CaptureRequest::ACCEPT_ON_SELECT);
            if (!clipboard && !raw && path.isEmpty() && !printGeometry &&
                !pin) {
                req.addSaveTask();
            }
        }
        int guiExitCode = requestCaptureAndWait(req);
        return guiExitCode;
    } else if (parser.isSet(fullArgument)) { // FULL

        // Option values
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);

        CaptureRequest req(CaptureRequest::FULLSCREEN_MODE, delay);
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (!clipboard && path.isEmpty() && !raw) {
            req.addSaveTask();
        }
        {
            int fullExitCode = requestCaptureAndWait(req);
            return fullExitCode;
        }
    } else if (parser.isSet(screenArgument)) { // SCREEN

        QString numberStr = parser.value(screenNumberOption);
        // Option values
        int screenNumber =
          numberStr.startsWith(QLatin1String("-")) ? -1 : numberStr.toInt();
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        QString region = parser.value(regionOption);
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);
        bool pin = parser.isSet(pinOption);
        bool edit = parser.isSet(editOption);

        CaptureRequest req(CaptureRequest::SCREEN_MODE, delay, screenNumber);
        if (edit) {
            req = CaptureRequest(CaptureRequest::GRAPHICAL_MODE, delay);
            if (screenNumber >= 0) {
                req.setSelectedMonitor(screenNumber);
            }
        }

        if (!region.isEmpty()) {
            if (region.startsWith("screen")) {
                AbstractLogger::error()
                  << "The 'screen' command does not support "
                     "'--region screen<N>'.\n"
                     "See flameshot --help.\n";
                exit(1);
            }
            req.setInitialSelection(Region().value(region).toRect());
        }
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (pin) {
            req.addTask(CaptureRequest::PIN);
        }

        if (!edit && !clipboard && !raw && path.isEmpty() && !pin) {
            req.addSaveTask();
        }

        {
            int screenExitCode = requestCaptureAndWait(req);
            return screenExitCode;
        }
    } else if (parser.isSet(configArgument)) { // CONFIG
        bool autostart = parser.isSet(autostartOption);
        bool notification = parser.isSet(notificationOption);
        bool filename = parser.isSet(filenameOption);
        bool tray = parser.isSet(trayOption);
        bool mainColor = parser.isSet(mainColorOption);
        bool contrastColor = parser.isSet(contrastColorOption);
        bool check = parser.isSet(checkOption);
        bool someFlagSet = (autostart || notification || filename || tray ||
                            mainColor || contrastColor || check);
        if (check) {
            AbstractLogger err = AbstractLogger::error(AbstractLogger::Stderr);
            bool ok = ConfigHandler().checkForErrors(&err);
            if (ok) {
                AbstractLogger::info()
                  << QStringLiteral("No errors detected.\n");
                goto finish;
            } else {
                return 1;
            }
        }
        if (!someFlagSet) {
            // Open gui when no options are given
#if defined(USE_KDSINGLEAPPLICATION)
            if (!claimSingleInstance(QStringLiteral("config"))) {
                return 0;
            }
#endif
            QObject::connect(
              qApp, &QApplication::lastWindowClosed, qApp, &QApplication::quit);
            Flameshot::instance()->config();
            qApp->exec();
        } else {
            ConfigHandler config;

            if (autostart) {
                config.setStartupLaunch(parser.value(autostartOption) ==
                                        "true");
            }
            if (notification) {
                config.setShowDesktopNotification(
                  parser.value(notificationOption) == "true");
            }
            if (filename) {
                QString newFilename(parser.value(filenameOption));
                config.setFilenamePattern(newFilename);
                FileNameHandler fh;
                QTextStream(stdout)
                  << QStringLiteral("The new pattern is '%1'\n"
                                    "Parsed pattern example: %2\n")
                       .arg(newFilename, fh.parsedPattern());
            }
            if (tray) {
                config.setDisabledTrayIcon(parser.value(trayOption) == "false");
            }
            if (mainColor) {
                // TODO use value handler
                QString colorCode = parser.value(mainColorOption);
                QColor parsedColor(colorCode);
                config.setUiColor(parsedColor);
            }
            if (contrastColor) {
                QString colorCode = parser.value(contrastColorOption);
                QColor parsedColor(colorCode);
                config.setContrastUiColor(parsedColor);
            }
        }
    }
finish:
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("process exit: pid=%1 code=0 (command line)")
           .arg(QCoreApplication::applicationPid());
    // The QApplication is intentionally left alive: the process is ending and
    // deleting it here used to run the static singletons (Flameshot, the
    // daemon, QHotkey) against a dead application object.
    return 0;
}
