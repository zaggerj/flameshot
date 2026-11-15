// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Flameshot uninstaller.
//
// The installer embeds a copy of this binary as RCDATA and drops it next to
// the application as uninstall.exe. Keeping it separate matters: the setup
// binary carries the whole 35 MB payload, and shipping that twice would double
// the footprint on disk for no reason.
//
// Everything here is plain Win32 with no C++ runtime dependencies, so the
// uninstaller still runs after the app directory has been emptied.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const wchar_t* kAppName = L"Flameshot";
const wchar_t* kExeName = L"flameshot.exe";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kUninstallKey =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Flameshot";
const wchar_t* kAppPathsKey =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\flameshot.exe";

bool g_silent = false;
int g_exitCode = 0;
bool g_relaunched = false;

std::wstring g_installDir;

void logLine(const wchar_t* text)
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
    char utf8[4096] = { 0 };
    const int bytes = WideCharToMultiByte(
      CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD written = 0;
    if (bytes > 1) {
        WriteFile(handle, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
        WriteFile(handle, "\r\n", 2, &written, nullptr);
    }
    CloseHandle(handle);
}

std::wstring parentOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    std::wstring parent = path.substr(0, slash);
    while (!parent.empty() &&
           (parent.back() == L'\\' || parent.back() == L'/')) {
        parent.pop_back();
    }
    return parent;
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return std::wstring();
}

void stopRunningInstances(const std::wstring& installDir)
{
    const std::wstring exe = installDir + L"\\" + kExeName;
    // Ask the daemon to leave on its own terms first: it hides the tray icon
    // on exit, whereas TerminateProcess leaves a dead icon in the notification
    // area that the shell keeps showing.
    {
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
            Sleep(600);
        }
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool killed = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, kExeName) != 0) {
                    continue;
                }
                HANDLE process =
                  OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                if (process == nullptr) {
                    continue;
                }
                // Only stop the copy that lives in the directory we own; a
                // development build elsewhere must be left alone.
                wchar_t image[MAX_PATH * 2] = { 0 };
                DWORD size = sizeof(image) - sizeof(wchar_t);
                if (QueryFullProcessImageNameW(process, 0, image, &size)) {
                    if (_wcsicmp(image, exe.c_str()) == 0) {
                        TerminateProcess(process, 0);
                        WaitForSingleObject(process, 3000);
                        killed = true;
                    }
                }
                CloseHandle(process);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!killed) {
            return;
        }
    }
}

bool deleteTree(const std::wstring& dir)
{
    wchar_t pattern[MAX_PATH * 2] = { 0 };
    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%s\\*", dir.c_str());
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return RemoveDirectoryW(dir.c_str()) != FALSE;
    }
    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring full = dir + L"\\" + data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            deleteTree(full);
        } else {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

void deleteRegValue(HKEY root, const wchar_t* subKey, const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

void deleteRegKey(HKEY root, const wchar_t* subKey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    RegDeleteKeyW(key, L"");
    RegCloseKey(key);
}

void run()
{
    wchar_t self[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const std::wstring selfPath(self);
    if (g_installDir.empty()) {
        g_installDir = parentOf(selfPath);
    }

    if (!g_silent) {
        const std::wstring text =
          L"将删除 Flameshot 的程序文件与快捷方式，已保存的截图不会被删除。\n\n"
          L"继续卸载？";
        if (MessageBoxW(nullptr,
                        text.c_str(),
                        L"卸载 Flameshot",
                        MB_ICONQUESTION | MB_YESNO) != IDYES) {
            return;
        }
    }

    logLine((L"uninstall -> " + g_installDir).c_str());
    stopRunningInstances(g_installDir);

    DeleteFileW((knownFolder(FOLDERID_Programs) + L"\\Flameshot.lnk").c_str());
    DeleteFileW((knownFolder(FOLDERID_Desktop) + L"\\Flameshot.lnk").c_str());
    deleteRegValue(HKEY_CURRENT_USER, kRunKey, L"Flameshot");
    deleteRegKey(HKEY_CURRENT_USER, kAppPathsKey);
    deleteRegKey(HKEY_CURRENT_USER, kUninstallKey);

    // This binary deletes its own directory, so remove everything except
    // itself first and hand the last step to the session manager.
    wchar_t pattern[MAX_PATH * 2] = { 0 };
    _snwprintf_s(pattern,
                 _countof(pattern),
                 _TRUNCATE,
                 L"%s\\*",
                 g_installDir.c_str());
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 ||
                wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring full = g_installDir + L"\\" + data.cFileName;
            if (_wcsicmp(full.c_str(), selfPath.c_str()) == 0) {
                continue;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                deleteTree(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    // Anything left over (a locked DLL, a file another process holds open) is
    // scheduled for deletion on the next reboot instead of failing loudly.
    // This copy normally runs from %TEMP% (see relaunchFromTemp), so the
    // directory it deletes does not contain it and can go away completely.
    // Should it still be running from the install directory, schedule the
    // last file for deletion at the next boot instead of leaving it behind.
    const bool selfInsideTarget =
      _wcsicmp(parentOf(selfPath).c_str(), g_installDir.c_str()) == 0;
    if (selfInsideTarget) {
        // Cannot delete the running image: hand it to the session manager.
        MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    RemoveDirectoryW(g_installDir.c_str());
    if (!selfInsideTarget) {
        // The %TEMP% copy goes away once this process has exited.
        MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    const bool cleaned = GetFileAttributesW(g_installDir.c_str()) ==
                         INVALID_FILE_ATTRIBUTES;
    logLine(cleaned ? L"done: removed" : L"done: scheduled for reboot");

    if (!g_silent) {
        MessageBoxW(nullptr,
                    L"Flameshot 已卸载。",
                    L"卸载完成",
                    MB_ICONINFORMATION);
    }
}

} // namespace

// Windows refuses to delete the image of a running process, so copy this
// executable to %TEMP% and let that copy remove the install directory. The
// original then exits immediately and every file can be deleted at once,
// without waiting for a reboot.
bool relaunchFromTemp()
{
    wchar_t self[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    wchar_t temp[MAX_PATH] = { 0 };
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        return false;
    }
    wchar_t name[64] = { 0 };
    _snwprintf_s(name,
                 _countof(name),
                 _TRUNCATE,
                 L"flameshot-uninstall-%lu.exe",
                 GetCurrentProcessId());
    const std::wstring target = std::wstring(temp) + name;
    if (!CopyFileW(self, target.c_str(), FALSE)) {
        return false;
    }

    std::wstring command = L"\"" + target + L"\" --relaunched";
    wchar_t self2[MAX_PATH * 2] = { 0 };
    GetModuleFileNameW(nullptr, self2, MAX_PATH * 2);
    command += L" --target=\"" + parentOf(self2) + L"\"";
    if (g_silent) {
        command += L" --silent";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(nullptr,
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        DETACHED_PROCESS,
                        nullptr,
                        nullptr,
                        &startup,
                        &info)) {
        DeleteFileW(target.c_str());
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    for (int i = 1; i < argumentCount; ++i) {
        const std::wstring argument(arguments[i]);
        if (_wcsicmp(argument.c_str(), L"--silent") == 0 ||
            _wcsicmp(argument.c_str(), L"/S") == 0) {
            g_silent = true;
        } else if (argument == L"--relaunched") {
            g_relaunched = true;
        } else if (argument.rfind(L"--target=", 0) == 0) {
            g_installDir = argument.substr(9);
            for (wchar_t& character : g_installDir) {
                if (character == L'/') {
                    character = L'\\';
                }
            }
            while (g_installDir.size() > 3 &&
                   g_installDir.back() == L'\\') {
                g_installDir.pop_back();
            }
        }
    }
    if (arguments != nullptr) {
        LocalFree(arguments);
    }

    if (!g_relaunched && relaunchFromTemp()) {
        CoUninitialize();
        return 0;
    }

    run();

    CoUninitialize();
    return g_exitCode;
}
