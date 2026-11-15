// SPDX-License-Identifier: GPL-3.0-or-later

#include "crashhandler.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#if defined(Q_OS_WIN)
#include <io.h>
#include <process.h>
#include <windows.h>
// clang-format off
#include <dbghelp.h>
#include <fcntl.h>
// clang-format on
#endif

namespace {

QString g_logDir;
QString g_logFile;
QFile g_log;
QMutex g_logMutex;
bool g_installed = false;

#if defined(Q_OS_WIN)
char g_crashDirUtf8[MAX_PATH * 3] = { 0 };
char g_logPathUtf8[MAX_PATH * 3] = { 0 };
volatile LONG g_inFilter = 0;

void writeAll(int fd, const char* text)
{
    const int len = static_cast<int>(strlen(text));
    int written = 0;
    while (written < len) {
        const int chunk = _write(fd, text + written, len - written);
        if (chunk <= 0) {
            break;
        }
        written += chunk;
    }
}

std::string moduleName(void* address, DWORD64* offsetOut)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address),
                            &module)) {
        return {};
    }
    wchar_t path[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return {};
    }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    char utf8[MAX_PATH * 3] = { 0 };
    WideCharToMultiByte(
      CP_UTF8, 0, base, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (offsetOut) {
        *offsetOut = reinterpret_cast<DWORD64>(address) -
                     reinterpret_cast<DWORD64>(module);
    }
    return std::string(utf8);
}

/// Write a short report for failures that never reach the SEH filter:
/// `abort()`, `qFatal`, a failed `std::terminate` (an exception escaping a
/// slot), or a raw SIGSEGV raised by a third party library. Without this the
/// process simply disappears, which is exactly the "the tray icon vanished
/// and I cannot tell whether it crashed" report.
void writeTextReport(const char* reason, long code)
{
    if (InterlockedExchange(&g_inFilter, 1) != 0) {
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    char stamp[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(stamp,
             sizeof(stamp),
             "%04d%02d%02d-%02d%02d%02d",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond);

    char path[MAX_PATH * 3 + 8];
    snprintf(path,
             sizeof(path),
             "%s\\abort-%s-%lu.txt",
             g_crashDirUtf8,
             stamp,
             static_cast<unsigned long>(pid));

    char report[512];
    snprintf(report,
             sizeof(report),
             "flameshot abnormal exit\n"
             "local time    : %04d-%02d-%02d %02d:%02d:%02d\n"
             "pid           : %lu\n"
             "reason        : %s\n"
             "code          : %ld (0x%08lX)\n"
             "version       : " APP_VERSION "\n"
             "log           : %s\n",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond,
             static_cast<unsigned long>(pid),
             reason,
             code,
             static_cast<unsigned long>(code),
             g_logPathUtf8);

    const int fd = _open(path, _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
    if (fd >= 0) {
        writeAll(fd, report);
        _close(fd);
    }
}

/// Unhandled C++ exception, e.g. one thrown out of a Qt slot.
void terminateHandler()
{
    writeTextReport("std::terminate (unhandled C++ exception)", 0);
    _exit(3);
}

/// The CRT raises these for `abort()`, `qFatal()` and for some third party
/// libraries that fault outside SEH. Report, then leave immediately: the
/// default handler would open a dialog or dump a second time.
void signalHandler(int sig)
{
    const char* name = "signal";
    switch (sig) {
        case SIGABRT:
            name = "SIGABRT (abort / qFatal)";
            break;
        case SIGSEGV:
            name = "SIGSEGV";
            break;
        case SIGFPE:
            name = "SIGFPE";
            break;
        case SIGILL:
            name = "SIGILL";
            break;
        case SIGTERM:
            name = "SIGTERM";
            break;
        default:
            break;
    }
    writeTextReport(name, sig);
    _exit(3);
}

/// Runs on the faulting thread with an unknown stack state: no Qt, no heap.
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info)
{
    if (InterlockedExchange(&g_inFilter, 1) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();
    const EXCEPTION_RECORD* er =
      info ? info->ExceptionRecord : nullptr;
    const DWORD code = er ? er->ExceptionCode : 0;
    void* address = er ? er->ExceptionAddress : nullptr;

    char stamp[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(stamp,
             sizeof(stamp),
             "%04d%02d%02d-%02d%02d%02d",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond);

    char basePath[MAX_PATH * 3];
    snprintf(basePath,
             sizeof(basePath),
             "%s\\crash-%s-%lu",
             g_crashDirUtf8,
             stamp,
             static_cast<unsigned long>(pid));

    char txtPath[MAX_PATH * 3 + 8];
    snprintf(txtPath, sizeof(txtPath), "%s.txt", basePath);

    DWORD64 offset = 0;
    const std::string module = moduleName(address, &offset);

    char report[1024];
    snprintf(report,
             sizeof(report),
             "flameshot crash report\n"
             "local time    : %04d-%02d-%02d %02d:%02d:%02d\n"
             "pid/thread    : %lu / %lu\n"
             "exception     : 0x%08lX\n"
             "address       : %p\n"
             "module        : %s+0x%llX\n"
             "version       : " APP_VERSION "\n"
             "qt            : " QT_VERSION_STR "\n"
             "log           : %s\n",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond,
             static_cast<unsigned long>(pid),
             static_cast<unsigned long>(tid),
             static_cast<unsigned long>(code),
             address,
             module.empty() ? "(unknown)" : module.c_str(),
             static_cast<unsigned long long>(offset),
             g_logPathUtf8);

    const int fd = _open(txtPath, _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
    if (fd >= 0) {
        writeAll(fd, report);
        _close(fd);
    }

    char dmpPath[MAX_PATH * 3 + 8];
    snprintf(dmpPath, sizeof(dmpPath), "%s.dmp", basePath);
    HANDLE file = CreateFileA(dmpPath,
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = tid;
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(),
                          pid,
                          file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                     MiniDumpNormal),
                          info ? &mei : nullptr,
                          nullptr,
                          nullptr);
        CloseHandle(file);
    }

    // No WER dialog: the evidence is on disk and the tray must not be left
    // with a ghost icon waiting for the user to click "close the program".
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void rotateIfNeeded()
{
    QFileInfo info(g_logFile);
    if (info.exists() && info.size() > 4 * 1024 * 1024) {
        const QString previous = g_logFile + ".1";
        QFile::remove(previous);
        QFile::rename(g_logFile, previous);
    }
}

void messageHandler(QtMsgType type,
                    const QMessageLogContext& context,
                    const QString& message)
{
    const char* level = "info";
    switch (type) {
        case QtDebugMsg:
            level = "debug";
            break;
        case QtInfoMsg:
            level = "info";
            break;
        case QtWarningMsg:
            level = "warning";
            break;
        case QtCriticalMsg:
            level = "critical";
            break;
        case QtFatalMsg:
            level = "fatal";
            break;
    }

    QString line = QStringLiteral("%1 [%2] %3")
                     .arg(QDateTime::currentDateTime().toString(
                            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                          QString::fromLatin1(level),
                          message);
    if (context.file) {
        line += QStringLiteral(" (%1:%2)")
                  .arg(QString::fromUtf8(context.file))
                  .arg(context.line);
    }

    {
        QMutexLocker locker(&g_logMutex);
        if (g_log.isOpen()) {
            rotateIfNeeded();
            g_log.write(line.toUtf8() + '\n');
            g_log.flush();
        }
    }

#if defined(Q_OS_WIN)
    OutputDebugStringW(
      (line + QStringLiteral("\n")).toStdWString().c_str());
#endif

    // Keep the console behaviour when one is attached; GUI launches have no
    // stderr and fprintf would fail silently.
    if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace

namespace CrashHandler {

QString logDirectory()
{
    if (!g_logDir.isEmpty()) {
        return g_logDir;
    }
    QString base =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    g_logDir = base + QStringLiteral("/Flameshot/logs");
    QDir().mkpath(g_logDir);
    QDir().mkpath(g_logDir + QStringLiteral("/crashes"));
    return g_logDir;
}

QString logFilePath()
{
    if (g_logFile.isEmpty()) {
        g_logFile = logDirectory() + QStringLiteral("/flameshot.log");
    }
    return g_logFile;
}

void install()
{
    if (g_installed) {
        return;
    }
    g_installed = true;

    logDirectory();
    logFilePath();
    g_log.setFileName(g_logFile);
    if (!g_log.open(QIODevice::WriteOnly | QIODevice::Append |
                    QIODevice::Text)) {
        // Logging is best effort: a read-only install location must not stop
        // the application from running.
        qWarning("Flameshot: cannot write the log file at %s",
                 qPrintable(g_logFile));
    }

    qInstallMessageHandler(messageHandler);

#if defined(Q_OS_WIN)
    const QString crashDir = logDirectory() + QStringLiteral("/crashes");
    const QByteArray native = QDir::toNativeSeparators(crashDir).toUtf8();
    qstrncpy(g_crashDirUtf8,
             native.constData(),
             sizeof(g_crashDirUtf8) - 1);
    const QByteArray logNative = QDir::toNativeSeparators(g_logFile).toUtf8();
    qstrncpy(g_logPathUtf8,
             logNative.constData(),
             sizeof(g_logPathUtf8) - 1);

    // Symbol names for the report; harmless if the DLL is unavailable.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    SetUnhandledExceptionFilter(crashFilter);

    // Crashes that never produce an SEH exception record still have to leave
    // evidence: an exception escaping a slot (`std::terminate`), `qFatal`, and
    // `abort()` all used to kill the process without a single line in the log,
    // which made "the tray icon disappeared" impossible to explain.
    std::set_terminate(terminateHandler);
    signal(SIGABRT, signalHandler);
    signal(SIGSEGV, signalHandler);
    signal(SIGFPE, signalHandler);
    signal(SIGILL, signalHandler);
    signal(SIGTERM, signalHandler);
#endif
}

} // namespace CrashHandler
