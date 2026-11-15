// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors

#pragma once

#include <QWidget>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace FlameshotWindow {

// A process that answers a global hotkey is not automatically allowed to take
// the foreground window, and a capture overlay without focus ignores Esc and
// Ctrl+C. Attaching to the foreground thread lifts that restriction for the
// duration of the call.
inline void forceForeground(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
#if defined(Q_OS_WIN)
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
      foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD ownThread = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 &&
                          foregroundThread != ownThread &&
                          AttachThreadInput(foregroundThread, ownThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (attached) {
        AttachThreadInput(foregroundThread, ownThread, FALSE);
    }
#endif
    widget->activateWindow();
    widget->raise();
}

} // namespace FlameshotWindow
