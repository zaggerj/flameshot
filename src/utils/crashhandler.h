// SPDX-License-Identifier: GPL-3.0-or-later
// Persistent diagnostics for the Windows build.
//
// A tray-resident screenshot tool dies silently: nothing appears in the event
// log unless WER happens to be enabled, and the user is left with "it closed
// by itself". This installs a Qt message handler plus an unhandled-exception
// filter that always leaves evidence on disk:
//
//   %LOCALAPPDATA%\Flameshot\logs\flameshot.log      (rolling, 4 MB)
//   %LOCALAPPDATA%\Flameshot\logs\crashes\*.txt      (exception details)
//   %LOCALAPPDATA%\Flameshot\logs\crashes\*.dmp      (minidump, if dbghelp
//                                                     can write one)
#pragma once

#include <QString>

namespace CrashHandler {

/// Install the message handler and the crash filter. Safe to call once,
/// right after the QApplication exists.
void install();

/// Directory that holds the rolling log and the crash reports.
QString logDirectory();

/// Absolute path of the rolling log file.
QString logFilePath();

} // namespace CrashHandler
