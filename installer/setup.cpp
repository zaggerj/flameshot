// SPDX-License-Identifier: GPL-3.0-or-later
// Flameshot installer / uninstaller.
//
// Deliberately plain Win32 + zlib: the installer must run before anything is
// installed, so it cannot depend on the Qt runtime it is about to unpack.
// Everything the application needs is embedded as a compressed archive
// (see tools/make_payload.py) and written to the target directory.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <zlib.h>

#include "resource.h"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace {

const wchar_t* kAppName = L"Flameshot";
const wchar_t* kVersion = L"15.0.0";
const wchar_t* kExeName = L"flameshot.exe";
const wchar_t* kUninstallerName = L"uninstall.exe";

HINSTANCE g_hinst = nullptr;
HWND g_main = nullptr;
HWND g_title = nullptr;
HWND g_subtitle = nullptr;
HWND g_pathLabel = nullptr;
HWND g_path = nullptr;
HWND g_browse = nullptr;
HWND g_desktopShortcut = nullptr;
HWND g_autostart = nullptr;
HWND g_launchAfter = nullptr;
HWND g_progress = nullptr;
HWND g_status = nullptr;
HWND g_install = nullptr;
HWND g_cancel = nullptr;

HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
bool g_uninstall = false;
bool g_busy = false;
int g_dpi = 96;

// Unattended mode. `Setup.exe --silent [--dir=<path>] [--no-desktop]
// [--no-autostart] [--launch]` installs without a single click, which is what
// makes the package deployable in bulk *and* verifiable by a test script.
bool g_silent = false;
bool g_optDesktop = true;
bool g_optAutostart = true;
bool g_optLaunch = false;
int g_exitCode = 0;
std::wstring g_silentTarget;

struct Entry
{
    std::wstring path;
    DWORD rawSize = 0;
    DWORD compSize = 0;
    const BYTE* data = nullptr;
};

std::vector<Entry> g_entries;
const BYTE* g_payload = nullptr;

// GetDpiForWindow / GetDpiForSystem only exist on Windows 10 1607+. Linking
// them directly would make the installer refuse to start with "entry point not
// found" on anything older, so resolve them at runtime and fall back to the
// screen DC metrics.
typedef UINT(WINAPI* PfnGetDpiForWindow)(HWND);
typedef UINT(WINAPI* PfnGetDpiForSystem)();

UINT dpiFromScreenDC()
{
    UINT dpi = 96;
    HDC screenDC = GetDC(nullptr);
    if (screenDC != nullptr) {
        const int value = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        if (value > 0) {
            dpi = static_cast<UINT>(value);
        }
    }
    return dpi;
}

UINT dpiForWindow(HWND hwnd)
{
    static PfnGetDpiForWindow fn = reinterpret_cast<PfnGetDpiForWindow>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn != nullptr) {
        const UINT dpi = fn(hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }
    return dpiFromScreenDC();
}

UINT dpiForSystem()
{
    static PfnGetDpiForSystem fn = reinterpret_cast<PfnGetDpiForSystem>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (fn != nullptr) {
        const UINT dpi = fn();
        if (dpi != 0) {
            return dpi;
        }
    }
    return dpiFromScreenDC();
}

int S(int value)
{
    return MulDiv(value, g_dpi, 96);
}

DWORD rd32(const BYTE*& cursor)
{
    DWORD value = 0;
    memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return value;
}

bool parsePayload()
{
    HRSRC res = FindResourceW(g_hinst,
                              MAKEINTRESOURCEW(IDR_PAYLOAD),
                              MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (res == nullptr) {
        return false;
    }
    HGLOBAL loaded = LoadResource(g_hinst, res);
    if (loaded == nullptr) {
        return false;
    }
    const BYTE* data = static_cast<const BYTE*>(LockResource(loaded));
    const DWORD size = SizeofResource(g_hinst, res);
    if (data == nullptr || size < 12) {
        return false;
    }
    if (memcmp(data, "FLPAY1\n\0", 8) != 0) {
        return false;
    }

    const BYTE* cursor = data + 8;
    const DWORD count = rd32(cursor);
    g_entries.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        const DWORD pathLen = rd32(cursor);
        if (pathLen == 0 || pathLen > 4096) {
            return false;
        }
        std::string utf8(reinterpret_cast<const char*>(cursor), pathLen);
        cursor += pathLen;
        Entry entry;
        const int wideLen = MultiByteToWideChar(
          CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        entry.path.resize(wideLen);
        MultiByteToWideChar(CP_UTF8,
                            0,
                            utf8.c_str(),
                            static_cast<int>(utf8.size()),
                            &entry.path[0],
                            wideLen);
        entry.rawSize = rd32(cursor);
        entry.compSize = rd32(cursor);
        g_entries.push_back(entry);
    }
    for (Entry& entry : g_entries) {
        if (cursor + entry.compSize > data + size) {
            return false;
        }
        entry.data = cursor;
        cursor += entry.compSize;
    }
    g_payload = data;
    return true;
}

bool inflateEntry(const Entry& entry, std::vector<BYTE>& out)
{
    out.resize(entry.rawSize);
    if (entry.rawSize == 0) {
        return true;
    }
    if (entry.compSize == entry.rawSize) { // stored
        memcpy(out.data(), entry.data, entry.rawSize);
        return true;
    }
    z_stream stream{};
    // 15 + 32 lets zlib auto-detect a zlib or gzip header, which is what
    // tools/make_payload.py writes (zlib.compress -> 0x78 0xDA).
    if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK) {
        return false;
    }
    stream.next_in = const_cast<Bytef*>(entry.data);
    stream.avail_in = entry.compSize;
    stream.next_out = out.data();
    stream.avail_out = entry.rawSize;
    const int result = inflate(&stream, Z_FINISH);
    const DWORD produced = stream.total_out;
    inflateEnd(&stream);
    return result == Z_STREAM_END && produced == entry.rawSize;
}

void pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void report(int percent, const std::wstring& what)
{
    if (g_progress) {
        SendMessageW(g_progress, PBM_SETPOS, percent, 0);
    }
    if (g_status) {
        SetWindowTextW(g_status, what.c_str());
    }
    pump();
}

std::wstring defaultInstallDir()
{
    wchar_t buffer[MAX_PATH * 2] = { 0 };
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (len == 0) {
        return L"C:\\Program Files\\Flameshot";
    }
    std::wstring dir(buffer);
    dir += L"\\Programs\\Flameshot";
    return dir;
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (SUCCEEDED(
          SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return {};
}

std::wstring getWindowText(HWND control)
{
    const int len = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(len), L'\0');
    GetWindowTextW(control, &text[0], len + 1);
    return text;
}

bool ensureDirectory(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring parentOf(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

bool writeEntry(const std::wstring& target, const Entry& entry)
{
    const std::wstring full = target + L"\\" + entry.path;
    // Create the intermediate directories.
    std::wstring partial = target;
    size_t start = 0;
    while (true) {
        const size_t slash = entry.path.find_first_of(L"\\/", start);
        if (slash == std::wstring::npos) {
            break;
        }
        partial += L"\\" + entry.path.substr(start, slash - start);
        ensureDirectory(partial);
        start = slash + 1;
    }

    std::vector<BYTE> data;
    if (!inflateEntry(entry, data)) {
        return false;
    }
    HANDLE file = CreateFileW(full.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = data.empty() ||
                    (WriteFile(file,
                               data.data(),
                               static_cast<DWORD>(data.size()),
                               &written,
                               nullptr) &&
                     written == data.size());
    CloseHandle(file);
    return ok;
}

// True while a copy of our binary that lives in `wanted` (lower-case full path)
// is still running.
bool instanceStillRunning(const std::wstring& wanted)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kExeName) != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE,
                                         entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            wchar_t path[MAX_PATH * 2] = { 0 };
            DWORD size = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                std::wstring actual(path);
                for (wchar_t& ch : actual) {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                if (actual == wanted) {
                    found = true;
                }
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// Stop the installed copy before overwriting its files.
//
// Ask first: `flameshot.exe quit` makes the daemon hide its tray icon (which
// sends NIM_DELETE) and exit. Killing it instead leaves a dead entry in the
// notification area that Explorer keeps showing until the user hovers it —
// every upgrade used to add one more ghost icon, which is exactly the
// "托盘里有六个图标" report. Terminate only as a last resort.
void stopRunningInstances(const std::wstring& installDir)
{
    std::wstring wanted = installDir + L"\\" + kExeName;
    for (wchar_t& ch : wanted) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (!instanceStillRunning(wanted)) {
        return;
    }

    std::wstring exe = installDir + L"\\" + kExeName;
    std::wstring command = L"\"" + exe + L"\" quit";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (CreateProcessW(nullptr,
                       command.data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       CREATE_NO_WINDOW,
                       nullptr,
                       nullptr,
                       &startup,
                       &info)) {
        WaitForSingleObject(info.hProcess, 8000);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        for (int waited = 0; waited < 6000; waited += 200) {
            if (!instanceStillRunning(wanted)) {
                Sleep(200);
                return;
            }
            Sleep(200);
        }
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kExeName) != 0) {
                continue;
            }
            HANDLE process = OpenProcess(
              PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
              FALSE,
              entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            wchar_t path[MAX_PATH * 2] = { 0 };
            DWORD size = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                std::wstring actual(path);
                for (wchar_t& ch : actual) {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                if (actual == wanted) {
                    TerminateProcess(process, 0);
                }
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    Sleep(300);
}

bool createShortcut(const std::wstring& linkPath,
                    const std::wstring& target,
                    const std::wstring& arguments,
                    const std::wstring& description,
                    const std::wstring& iconPath)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
        return false;
    }
    link->SetPath(target.c_str());
    if (!arguments.empty()) {
        link->SetArguments(arguments.c_str());
    }
    if (!description.empty()) {
        link->SetDescription(description.c_str());
    }
    if (!iconPath.empty()) {
        link->SetIconLocation(iconPath.c_str(), 0);
    }
    link->SetWorkingDirectory(parentOf(target).c_str());

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

void setRegString(HKEY root,
                  const wchar_t* subKey,
                  const wchar_t* name,
                  const wchar_t* value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root,
                        subKey,
                        0,
                        nullptr,
                        0,
                        KEY_WRITE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key,
                   name,
                   0,
                   REG_SZ,
                   reinterpret_cast<const BYTE*>(value),
                   static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void setRegDword(HKEY root, const wchar_t* subKey, const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root,
                        subKey,
                        0,
                        nullptr,
                        0,
                        KEY_WRITE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key,
                   name,
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

void deleteRegValue(HKEY root, const wchar_t* subKey, const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

void deleteRegKey(HKEY root, const wchar_t* subKey)
{
    RegDeleteKeyW(root, subKey);
}

const wchar_t* kRunKey =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kUninstallKey =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Flameshot";
const wchar_t* kAppPathsKey =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\flameshot.exe";

std::wstring defaultIni()
{
    return L"[General]\r\n"
           L"showStartupLaunchMessage=false\r\n"
           L"checkForUpdates=false\r\n"
           L"startupLaunch=true\r\n"
           L"captureActiveMonitor=true\r\n"
           L"\r\n"
           L"[Shortcuts]\r\n"
           L"TAKE_SCREENSHOT=Ctrl+Alt+A\r\n"
           L"SCROLL_CAPTURE=Ctrl+Alt+S\r\n"
           L"HISTORY_PRTSC=Ctrl+Alt+H\r\n";
}

void writeTextFile(const std::wstring& path, const std::wstring& text)
{
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    // Qt reads and writes ini files as UTF-8 without a BOM. Writing the wide
    // characters verbatim produced a UTF-16 file that QSettings refused to
    // parse, which silently dropped the shortcut overrides below.
    const int bytes = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    DWORD written = 0;
    if (bytes > 0) {
        std::vector<char> utf8(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8,
                            0,
                            text.c_str(),
                            static_cast<int>(text.size()),
                            utf8.data(),
                            bytes,
                            nullptr,
                            nullptr);
        WriteFile(file, utf8.data(), static_cast<DWORD>(bytes), &written, nullptr);
    }
    CloseHandle(file);
}

// Drop a resource (currently only the standalone uninstaller) to disk.
bool writeResource(DWORD id, const std::wstring& path)
{
    HRSRC res =
      FindResourceW(g_hinst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (res == nullptr) {
        return false;
    }
    HGLOBAL loaded = LoadResource(g_hinst, res);
    if (loaded == nullptr) {
        return false;
    }
    const BYTE* data = static_cast<const BYTE*>(LockResource(loaded));
    const DWORD size = SizeofResource(g_hinst, res);
    if (data == nullptr || size == 0) {
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) &&
                    written == size;
    CloseHandle(file);
    return ok;
}

bool isChecked(HWND box)
{
    return SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void logLine(const std::wstring& text)
{
    wchar_t temp[MAX_PATH] = { 0 };
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        return;
    }
    const std::wstring file = std::wstring(temp) + L"FlameshotSetup.log";
    HANDLE handle = CreateFileW(file.c_str(),
                                FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[2048] = { 0 };
    _snwprintf_s(line,
                 _countof(line),
                 _TRUNCATE,
                 L"[%02d:%02d:%02d] %s\r\n",
                 now.wHour,
                 now.wMinute,
                 now.wSecond,
                 text.c_str());
    DWORD written = 0;
    const int bytes = WideCharToMultiByte(CP_UTF8,
                                         0,
                                         line,
                                         -1,
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (bytes > 0) {
        std::vector<char> utf8(static_cast<size_t>(bytes));
        WideCharToMultiByte(
          CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
        WriteFile(handle, utf8.data(), static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(handle);
}

// Single funnel for user-visible problems: modal in interactive mode, logged
// (and turned into a non-zero exit code) in unattended mode.
void alert(const wchar_t* text, const wchar_t* caption, UINT flags)
{
    logLine(std::wstring(caption) + L": " + text);
    if ((flags & MB_ICONERROR) != 0) {
        g_exitCode = 1;
    }
    if (!g_silent) {
        alert( text, caption, flags);
    }
}

bool confirm(HWND owner, const wchar_t* text, const wchar_t* caption)
{
    if (g_silent) {
        return true;
    }
    return MessageBoxW(owner, text, caption, MB_ICONQUESTION | MB_YESNO) ==
           IDYES;
}

std::wstring chosenPath()
{
    return g_silent ? g_silentTarget : getWindowText(g_path);
}

bool optDesktop()
{
    return g_silent ? g_optDesktop : isChecked(g_desktopShortcut);
}

bool optAutostart()
{
    return g_silent ? g_optAutostart : isChecked(g_autostart);
}

bool optLaunch()
{
    return g_silent ? g_optLaunch : isChecked(g_launchAfter);
}

std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* name)
{
    std::wstring value;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return value;
    }
    wchar_t buffer[1024] = { 0 };
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegQueryValueExW(key,
                         name,
                         nullptr,
                         &type,
                         reinterpret_cast<BYTE*>(buffer),
                         &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        value.assign(buffer);
    }
    RegCloseKey(key);
    return value;
}

void doInstall()
{
    const std::wstring target = chosenPath();
    if (target.empty()) {
        alert(
                    L"请选择安装位置。",
                    L"路径无效",
                    MB_ICONWARNING);
        return;
    }
    if (!ensureDirectory(target)) {
        alert(
                    L"无法创建安装目录，请换一个位置。",
                    L"安装失败",
                    MB_ICONERROR);
        return;
    }

    g_busy = true;
    EnableWindow(g_install, FALSE);
    EnableWindow(g_browse, FALSE);
    EnableWindow(g_path, FALSE);

    report(0, L"正在准备…");
    stopRunningInstances(target);

    int done = 0;
    for (const Entry& entry : g_entries) {
        const int percent = 100 * ++done / static_cast<int>(g_entries.size());
        report(percent,
               std::wstring(L"正在安装：") + entry.path);
        if (!writeEntry(target, entry)) {
            report(percent,
                   std::wstring(L"写入失败：") + entry.path);
            alert(
                        (L"无法写入文件：\n" + target + L"\\" + entry.path +
                         L"\n\n请确认该目录可写，并且没有程序正在使用这些文件。")
                          .c_str(),
                        L"安装失败",
                        MB_ICONERROR);
            g_busy = false;
            EnableWindow(g_install, TRUE);
            EnableWindow(g_browse, TRUE);
            EnableWindow(g_path, TRUE);
            return;
        }
    }

    const std::wstring exe = target + L"\\" + kExeName;
    const std::wstring ini = target + L"\\flameshot.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        writeTextFile(ini, defaultIni());
    }

    // Uninstaller: a separate 360 KB binary embedded as a resource. Copying
    // this executable instead would have shipped the 37 MB payload twice.
    const std::wstring uninstaller = target + L"\\" + kUninstallerName;
    DeleteFileW(uninstaller.c_str());
    if (!writeResource(IDR_UNINSTALLER, uninstaller)) {
        logLine(L"warning: could not write uninstall.exe");
    }

    const std::wstring icon = target + L"\\flameshot.ico";
    const std::wstring startMenu =
      knownFolder(FOLDERID_Programs) + L"\\Flameshot.lnk";
    const std::wstring desktop =
      knownFolder(FOLDERID_Desktop) + L"\\Flameshot.lnk";
    // `daemon` keeps the tray icon and the global hotkeys alive, which is what
    // people expect after double-clicking the icon. An earlier build used
    // `gui` here: that only ran one capture and quit, so the app looked like it
    // had no tray at all and every hotkey was dead afterwards.
    createShortcut(startMenu, exe, L"daemon", L"Flameshot 截图", icon);
    if (optDesktop()) {
        createShortcut(desktop, exe, L"daemon", L"Flameshot 截图", icon);
    } else {
        DeleteFileW(desktop.c_str());
    }

    setRegString(HKEY_CURRENT_USER, kAppPathsKey, nullptr, exe.c_str());
    setRegString(HKEY_CURRENT_USER, kAppPathsKey, L"Path", target.c_str());

    if (optAutostart()) {
        const std::wstring command = L"\"" + exe + L"\" daemon";
        setRegString(HKEY_CURRENT_USER, kRunKey, L"Flameshot", command.c_str());
    } else {
        deleteRegValue(HKEY_CURRENT_USER, kRunKey, L"Flameshot");
    }

    setRegString(HKEY_CURRENT_USER,
                 kUninstallKey,
                 L"DisplayName",
                 (std::wstring(kAppName) + L" " + kVersion).c_str());
    setRegString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", exe.c_str());
    setRegString(HKEY_CURRENT_USER,
                 kUninstallKey,
                 L"UninstallString",
                 (L"\"" + uninstaller + L"\"").c_str());
    setRegString(HKEY_CURRENT_USER,
                 kUninstallKey,
                 L"InstallLocation",
                 target.c_str());
    setRegString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kVersion);
    setRegString(HKEY_CURRENT_USER,
                 kUninstallKey,
                 L"Publisher",
                 L"Flameshot contributors");
    setRegDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    setRegDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);

    report(100, L"安装完成。");

    if (optLaunch()) {
        ShellExecuteW(nullptr, L"open", exe.c_str(), L"daemon", target.c_str(),
                      SW_SHOWNORMAL);
    }

    g_busy = false;
    EnableWindow(g_install, TRUE);
    EnableWindow(g_browse, TRUE);
    EnableWindow(g_path, TRUE);

    alert(
                (L"Flameshot 已安装到：\n" + target +
                 L"\n\n截图：Ctrl+Alt+A\n滚动截屏：Ctrl+Alt+S\n"
                 L"（如快捷键无反应，请先退出其它同类截图工具）")
                  .c_str(),
                L"安装完成",
                MB_ICONINFORMATION);
    DestroyWindow(g_main);
}

void doUninstall()
{
    if (!confirm(g_main,
                 L"将删除 Flameshot 的程序文件与快捷方式，"
                 L"已保存的截图不会被删除。\n\n继续卸载？",
                 L"确认卸载")) {
        return;
    }
    const std::wstring target = chosenPath();
    g_busy = true;
    EnableWindow(g_install, FALSE);
    EnableWindow(g_cancel, FALSE);

    report(0, L"正在卸载…");
    stopRunningInstances(target);

    DeleteFileW((knownFolder(FOLDERID_Programs) + L"\\Flameshot.lnk").c_str());
    DeleteFileW((knownFolder(FOLDERID_Desktop) + L"\\Flameshot.lnk").c_str());
    deleteRegValue(HKEY_CURRENT_USER, kRunKey, L"Flameshot");
    deleteRegKey(HKEY_CURRENT_USER, kAppPathsKey);
    deleteRegKey(HKEY_CURRENT_USER, kUninstallKey);

    wchar_t self[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const std::wstring selfPath(self);

    int done = 0;
    for (const Entry& entry : g_entries) {
        const std::wstring full = target + L"\\" + entry.path;
        report(100 * ++done / static_cast<int>(g_entries.size()),
               std::wstring(L"正在删除：") + entry.path);
        if (_wcsicmp(full.c_str(), selfPath.c_str()) == 0) {
            continue;
        }
        DeleteFileW(full.c_str());
    }

    // Remove directories bottom-up. Iterate a few times because the tree can
    // be several levels deep.
    for (int pass = 0; pass < 6; ++pass) {
        WIN32_FIND_DATAW data;
        const std::wstring pattern = target + L"\\*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            break;
        }
        bool removedAny = false;
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring dir = target + L"\\" + data.cFileName;
            if (RemoveDirectoryW(dir.c_str())) {
                removedAny = true;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        if (!removedAny) {
            break;
        }
    }

    if (_wcsicmp(parentOf(selfPath).c_str(), target.c_str()) == 0) {
        // The uninstaller lives inside the tree it just emptied: ask Windows
        // to remove it after the process exits.
        MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        RemoveDirectoryW(target.c_str());
    }

    alert(
                L"Flameshot 已卸载。",
                L"卸载完成",
                MB_ICONINFORMATION);
    DestroyWindow(g_main);
}

void browseForFolder()
{
    BROWSEINFOW info{};
    info.hwndOwner = g_main;
    info.lpszTitle = L"选择安装位置";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST item = SHBrowseForFolderW(&info);
    if (item == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH * 2] = { 0 };
    if (SHGetPathFromIDListW(item, path)) {
        SetWindowTextW(g_path, path);
    }
    CoTaskMemFree(item);
}

void layoutChildren(HWND hwnd)
{
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int margin = S(24);

    SetWindowPos(g_title, nullptr, margin, S(18), width - margin * 2, S(34),
                 SWP_NOZORDER);
    SetWindowPos(g_subtitle, nullptr, margin, S(54), width - margin * 2, S(44),
                 SWP_NOZORDER);
    SetWindowPos(g_pathLabel, nullptr, margin, S(106), width - margin * 2, S(20),
                 SWP_NOZORDER);
    SetWindowPos(g_path, nullptr, margin, S(128), width - margin * 2 - S(100),
                 S(26), SWP_NOZORDER);
    SetWindowPos(g_browse, nullptr, width - margin - S(88), S(126), S(88), S(28),
                 SWP_NOZORDER);

    int y = S(168);
    if (!g_uninstall) {
        for (HWND box : { g_desktopShortcut, g_autostart, g_launchAfter }) {
            SetWindowPos(box, nullptr, margin, y, width - margin * 2, S(22),
                         SWP_NOZORDER);
            y += S(26);
        }
    }

    SetWindowPos(g_progress, nullptr, margin, y + S(12), width - margin * 2,
                 S(18), SWP_NOZORDER);
    SetWindowPos(g_status, nullptr, margin, y + S(38), width - margin * 2,
                 S(40), SWP_NOZORDER);
    SetWindowPos(g_install, nullptr, width - margin - S(196), y + S(84), S(92),
                 S(30), SWP_NOZORDER);
    SetWindowPos(g_cancel, nullptr, width - margin - S(92), y + S(84), S(92),
                 S(30), SWP_NOZORDER);
}

HWND createControl(const wchar_t* cls,
                   const wchar_t* text,
                   DWORD style,
                   int id)
{
    return CreateWindowExW(0,
                           cls,
                           text,
                           WS_CHILD | WS_VISIBLE | style,
                           0,
                           0,
                           10,
                           10,
                           g_main,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           g_hinst,
                           nullptr);
}

void applyFonts()
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0)) {
        LOGFONTW font = metrics.lfMessageFont;
        font.lfHeight = -MulDiv(10, g_dpi, 72);
        g_font = CreateFontIndirectW(&font);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfHeight = -MulDiv(19, g_dpi, 72);
        titleFont.lfWeight = FW_SEMIBOLD;
        g_titleFont = CreateFontIndirectW(&titleFont);
    }
    if (g_font == nullptr) {
        g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    if (g_titleFont == nullptr) {
        g_titleFont = g_font;
    }

    EnumChildWindows(
      g_main,
      [](HWND child, LPARAM) -> BOOL {
          SendMessageW(child,
                       WM_SETFONT,
                       reinterpret_cast<WPARAM>(g_font),
                       TRUE);
          return TRUE;
      },
      0);
    SendMessageW(g_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont),
                 TRUE);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        case WM_CREATE: {
            g_main = hwnd;
            g_dpi = static_cast<int>(dpiForWindow(hwnd));
            if (g_dpi == 0) {
                g_dpi = 96;
            }

            g_title = createControl(
              L"STATIC",
              g_uninstall ? L"卸载 Flameshot"
                          : (std::wstring(L"安装 Flameshot ") + kVersion).c_str(),
              SS_LEFT,
              IDC_TITLE);
            g_subtitle = createControl(
              L"STATIC",
              g_uninstall
                ? L"将删除程序文件、开始菜单与桌面快捷方式、开机启动项。\r\n"
                  L"你保存的截图不会被删除。"
                : L"截图、标注、滚动截屏。安装到当前用户目录，不需要管理员权限；\r\n"
                  L"装好后按 Ctrl+Alt+A 截图，Ctrl+Alt+S 滚动截屏。",
              SS_LEFT,
              IDC_SUBTITLE);
            g_pathLabel =
              createControl(L"STATIC", L"安装位置", SS_LEFT, IDC_PATHLABEL);
            g_path = createControl(L"EDIT",
                                   L"",
                                   WS_BORDER | ES_AUTOHSCROLL,
                                   IDC_PATH);
            SetWindowTextW(g_path, defaultInstallDir().c_str());
            if (g_uninstall) {
                SendMessageW(g_path, EM_SETREADONLY, TRUE, 0);
            }
            g_browse = createControl(L"BUTTON",
                                     L"浏览…",
                                     BS_PUSHBUTTON | WS_TABSTOP,
                                     IDC_BROWSE);
            g_desktopShortcut = createControl(L"BUTTON",
                                              L"创建桌面快捷方式",
                                              BS_AUTOCHECKBOX | WS_TABSTOP,
                                              IDC_DESKTOPSHORTCUT);
            SendMessageW(g_desktopShortcut, BM_SETCHECK, BST_CHECKED, 0);
            g_autostart = createControl(
              L"BUTTON",
              L"开机自动启动（托盘常驻，热键随时可用）",
              BS_AUTOCHECKBOX | WS_TABSTOP,
              IDC_AUTOSTART);
            SendMessageW(g_autostart, BM_SETCHECK, BST_CHECKED, 0);
            g_launchAfter = createControl(L"BUTTON",
                                          L"安装完成后立即启动",
                                          BS_AUTOCHECKBOX | WS_TABSTOP,
                                          IDC_LAUNCHAFTER);
            SendMessageW(g_launchAfter, BM_SETCHECK, BST_CHECKED, 0);

            if (g_uninstall) {
                ShowWindow(g_desktopShortcut, SW_HIDE);
                ShowWindow(g_autostart, SW_HIDE);
                ShowWindow(g_launchAfter, SW_HIDE);
            }

            g_progress = createControl(PROGRESS_CLASSW,
                                       L"",
                                       0,
                                       IDC_PROGRESS);
            SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
            g_status = createControl(L"STATIC", L"", SS_LEFT, IDC_STATUS);
            g_install = createControl(L"BUTTON",
                                      g_uninstall ? L"卸载" : L"安装",
                                      BS_DEFPUSHBUTTON | WS_TABSTOP,
                                      IDC_INSTALL);
            g_cancel = createControl(
              L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDC_CANCEL);

            applyFonts();
            layoutChildren(hwnd);

            EnableWindow(g_install, TRUE);
            SetFocus(g_install);

            if (g_uninstall) {
                // The uninstaller lives in the directory it removes.
                wchar_t self[MAX_PATH * 2] = { 0 };
                GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
                SetWindowTextW(g_path, parentOf(self).c_str());
            }
            return 0;
        }
        // The checkboxes and labels sit on a white window, but the classic
        // button class paints its own grey background unless it is told which
        // brush to use. With visual styles active this is ignored, so it is a
        // no-op there and the fix everywhere else.
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HDC controlDC = reinterpret_cast<HDC>(wParam);
            SetBkMode(controlDC, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_INSTALL:
                    if (!g_busy) {
                        if (g_uninstall) {
                            doUninstall();
                        } else {
                            doInstall();
                        }
                    }
                    return 0;
                case IDC_CANCEL:
                    if (!g_busy) {
                        DestroyWindow(hwnd);
                    }
                    return 0;
                case IDC_BROWSE:
                    if (!g_busy) {
                        browseForFolder();
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            if (g_busy) {
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE,
                    PWSTR,
                    int showCommand)
{
    g_hinst = instance;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    wchar_t self[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const std::wstring selfPath(self);
    const size_t slash = selfPath.find_last_of(L"\\/");
    const std::wstring fileName =
      slash == std::wstring::npos ? selfPath : selfPath.substr(slash + 1);
    g_uninstall = _wcsicmp(fileName.c_str(), L"uninstall.exe") == 0;

    // Command line is parsed before anything else so that --silent also
    // suppresses the fatal "damaged package" dialog below.
    {
        int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        for (int i = 1; i < argumentCount; ++i) {
            const std::wstring argument(arguments[i]);
            if (_wcsicmp(argument.c_str(), L"--silent") == 0 ||
                _wcsicmp(argument.c_str(), L"/S") == 0) {
                g_silent = true;
            } else if (argument.rfind(L"--dir=", 0) == 0) {
                g_silentTarget = argument.substr(6);
            } else if (_wcsicmp(argument.c_str(), L"--no-desktop") == 0) {
                g_optDesktop = false;
            } else if (_wcsicmp(argument.c_str(), L"--no-autostart") == 0) {
                g_optAutostart = false;
            } else if (_wcsicmp(argument.c_str(), L"--launch") == 0) {
                g_optLaunch = true;
            }
        }
        if (arguments != nullptr) {
            LocalFree(arguments);
        }
    }

    // Normalise the target: forward slashes are legal for Win32 but they leak
    // into the registry and the generated command lines, so store one form.
    if (!g_silentTarget.empty()) {
        for (wchar_t& character : g_silentTarget) {
            if (character == L'/') {
                character = L'\\';
            }
        }
        while (g_silentTarget.size() > 3 && g_silentTarget.back() == L'\\') {
            g_silentTarget.pop_back();
        }
    }

    if (!parsePayload()) {
        if (!g_silent) {
            MessageBoxW(nullptr,
                        L"安装包已损坏（无法读取内置文件）。请重新下载。",
                        L"Flameshot 安装程序",
                        MB_ICONERROR);
        } else {
            logLine(L"error: payload is damaged or unreadable");
        }
        return 1;
    }

    if (g_silent) {
        if (g_uninstall) {
            if (g_silentTarget.empty()) {
                g_silentTarget = readRegString(
                  HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
            }
            if (g_silentTarget.empty()) {
                logLine(L"error: cannot determine install location");
                CoUninitialize();
                return 1;
            }
            logLine(L"uninstall -> " + g_silentTarget);
            doUninstall();
        } else {
            if (g_silentTarget.empty()) {
                g_silentTarget = defaultInstallDir();
            }
            logLine(L"install -> " + g_silentTarget);
            doInstall();
        }
        logLine(g_exitCode == 0 ? L"done: success" : L"done: failed");
        CoUninitialize();
        return g_exitCode;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
      reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon =
      LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    windowClass.lpszClassName = L"FlameshotSetupWindow";
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    const int dpi = static_cast<int>(dpiForSystem());
    const int width = MulDiv(580, dpi, 96);
    const int height = MulDiv(430, dpi, 96);

    HWND hwnd = CreateWindowExW(0,
                                windowClass.lpszClassName,
                                g_uninstall ? L"卸载 Flameshot"
                                            : L"Flameshot 安装程序",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                  WS_MINIMIZEBOX,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                width,
                                height,
                                nullptr,
                                nullptr,
                                instance,
                                nullptr);
    if (hwnd == nullptr) {
        return 1;
    }

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd,
                 nullptr,
                 (screenWidth - (rect.right - rect.left)) / 2,
                 (screenHeight - (rect.bottom - rect.top)) / 2,
                 0,
                 0,
                 SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    CoUninitialize();
    return g_exitCode;
}
