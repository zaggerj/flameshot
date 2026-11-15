// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "globalshortcutfilter.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"

#include <QDebug>
#include <QKeyCombination>
#include <QLatin1String>
#include <QString>

#include <qt_windows.h>

// Older MinGW winuser.h does not expose this flag (Windows 7+).
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

namespace {

enum HotkeyId
{
    CapturePrtSc = 1,
    HistoryPrtSc = 2,
    ScrollCapture = 3,
    TakeScreenshot = 4
};

struct ShortcutDefinition
{
    const char* action;
    int id;
};

// TAKE_SCREENSHOT is first on purpose: it is the main capture key (default
// Ctrl+Alt+A). CAPTURE_PRTSC is kept for configurations written by older
// builds, where the same chord lived under that name; a duplicated chord is
// registered once and the later entry is skipped instead of failing with
// error 1409.
const ShortcutDefinition definitions[] = {
    { "TAKE_SCREENSHOT", TakeScreenshot },
    { "CAPTURE_PRTSC", CapturePrtSc },
    { "HISTORY_PRTSC", HistoryPrtSc },
    { "SCROLL_CAPTURE", ScrollCapture },
};

// Qt releases use different portable names for Qt::Key_Print. Accept the
// spelling stored in the config even when QKeySequence cannot parse it.
QKeySequence parseSequence(const QString& text)
{
    if (text.isEmpty()) {
        return {};
    }

    QKeySequence sequence(text);
    if (!sequence.isEmpty()) {
        return sequence;
    }

    const bool isPrint = text.contains(QLatin1String("PrtSc"),
                                       Qt::CaseInsensitive) ||
                         text.contains(QLatin1String("Print"),
                                       Qt::CaseInsensitive);
    if (!isPrint) {
        return {};
    }

    int key = Qt::Key_Print;
    if (text.contains(QLatin1String("Shift"), Qt::CaseInsensitive)) {
        key |= Qt::SHIFT;
    }
    if (text.contains(QLatin1String("Ctrl"), Qt::CaseInsensitive)) {
        key |= Qt::CTRL;
    }
    if (text.contains(QLatin1String("Alt"), Qt::CaseInsensitive)) {
        key |= Qt::ALT;
    }
    if (text.contains(QLatin1String("Meta"), Qt::CaseInsensitive)) {
        key |= Qt::META;
    }
    return QKeySequence(key);
}

quint32 nativeModifiers(Qt::KeyboardModifiers modifiers)
{
    quint32 result = 0;
    if (modifiers & Qt::ControlModifier) {
        result |= MOD_CONTROL;
    }
    if (modifiers & Qt::ShiftModifier) {
        result |= MOD_SHIFT;
    }
    if (modifiers & Qt::AltModifier) {
        result |= MOD_ALT;
    }
    if (modifiers & Qt::MetaModifier) {
        result |= MOD_WIN;
    }
    // MOD_NOREPEAT keeps a held key from firing the capture repeatedly.
    return result | MOD_NOREPEAT;
}

quint32 nativeKeycode(int qtKey)
{
    // Qt key values for ASCII letters and digits match Windows VK_* values.
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return static_cast<quint32>('A' + (qtKey - Qt::Key_A));
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return static_cast<quint32>('0' + (qtKey - Qt::Key_0));
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return static_cast<quint32>(VK_F1 + (qtKey - Qt::Key_F1));
    }

    switch (qtKey) {
        case Qt::Key_Print:
            return VK_SNAPSHOT;
        case Qt::Key_Space:
            return VK_SPACE;
        case Qt::Key_Tab:
            return VK_TAB;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return VK_RETURN;
        case Qt::Key_Escape:
            return VK_ESCAPE;
        case Qt::Key_Backspace:
            return VK_BACK;
        case Qt::Key_Delete:
            return VK_DELETE;
        case Qt::Key_Insert:
            return VK_INSERT;
        case Qt::Key_Home:
            return VK_HOME;
        case Qt::Key_End:
            return VK_END;
        case Qt::Key_PageUp:
            return VK_PRIOR;
        case Qt::Key_PageDown:
            return VK_NEXT;
        case Qt::Key_Left:
            return VK_LEFT;
        case Qt::Key_Right:
            return VK_RIGHT;
        case Qt::Key_Up:
            return VK_UP;
        case Qt::Key_Down:
            return VK_DOWN;
        case Qt::Key_Pause:
            return VK_PAUSE;
        case Qt::Key_ScrollLock:
            return VK_SCROLL;
        default:
            return 0;
    }
}

} // namespace

GlobalShortcutFilter::GlobalShortcutFilter(QObject* parent)
  : QObject(parent)
{
    reloadShortcuts();
}

GlobalShortcutFilter::~GlobalShortcutFilter()
{
    unregisterAll();
}

void GlobalShortcutFilter::unregisterAll()
{
    for (const Binding& binding : m_bindings) {
        UnregisterHotKey(NULL, binding.id);
    }
    m_bindings.clear();
}

bool GlobalShortcutFilter::registerBinding(int id,
                                           const QKeySequence& sequence)
{
    const QKeyCombination combination = sequence[0];
    const quint32 vk = nativeKeycode(static_cast<int>(combination.key()));
    const quint32 mods = nativeModifiers(combination.keyboardModifiers());

    if (vk == 0) {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("Global shortcut %1 uses a key that cannot be "
                            "mapped to a Windows hotkey.")
               .arg(sequence.toString());
        return false;
    }

    if (!RegisterHotKey(NULL, id, mods, vk)) {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("Could not register global shortcut %1 - another "
                            "application probably owns it. This binding "
                            "stays disabled.")
               .arg(sequence.toString());
        return false;
    }

    m_bindings.append(Binding{ id });
    return true;
}

void GlobalShortcutFilter::reloadShortcuts()
{
    unregisterAll();

    ConfigHandler config;
    QVector<QKeySequence> registered;
    for (const ShortcutDefinition& definition : definitions) {
        const QString action = QString::fromLatin1(definition.action);
        const QKeySequence sequence = parseSequence(config.shortcut(action));
        if (sequence.isEmpty()) {
            // Not an error: an empty value is how a binding is disabled.
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Global shortcut %1 is disabled").arg(action);
            continue;
        }
        // Two actions may point at the same chord after an upgrade (the old
        // CAPTURE_PRTSC name and the new TAKE_SCREENSHOT default). Windows
        // rejects the second RegisterHotKey with error 1409, so keep the
        // first registration and ignore the duplicate.
        if (registered.contains(sequence)) {
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Global shortcut %1 shares %2 with an already "
                                "registered action; skipped")
                   .arg(action, sequence.toString());
            continue;
        }
        if (registerBinding(definition.id, sequence)) {
            registered.append(sequence);
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Registered global shortcut %1 as %2")
                   .arg(action, sequence.toString());
        }
    }
}

bool GlobalShortcutFilter::nativeEventFilter(const QByteArray& eventType,
                                             void* message,
                                             qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message != WM_HOTKEY) {
        return false;
    }

    switch (LOWORD(msg->wParam)) {
        case TakeScreenshot:
            emit captureRequested();
            return true;
        case CapturePrtSc:
            emit captureRequested();
            return true;
        case HistoryPrtSc:
            emit historyRequested();
            return true;
        case ScrollCapture:
            emit scrollCaptureRequested();
            return true;
        default:
            return false;
    }
}
