// Enumerate the Windows notification area with UI Automation and print, for
// every icon, the owning process id.
//
// Why this exists: a force-killed tray application leaves a "ghost" icon that
// Explorer keeps drawing until the pointer hovers it. The tray then shows
// several identical icons while only one process is alive, which reads as
// "the app started six times / it is unstable". Comparing the number of icons
// with the number of live processes is the only way to tell the two apart.
//
// Build:
//   g++ -O2 -municode trayprobe.cpp -o trayprobe.exe -lole32 -loleaut32 -luuid

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <uiautomation.h>
#include <cstdio>
#include <string>

static std::wstring rectToString(const RECT& r)
{
    wchar_t buf[128];
    swprintf(buf, 128, L"(%ld,%ld %ldx%ld)", r.left, r.top,
             r.right - r.left, r.bottom - r.top);
    return buf;
}

static BSTR getProp(IUIAutomationElement* element, PROPERTYID id)
{
    VARIANT v;
    VariantInit(&v);
    BSTR out = nullptr;
    if (SUCCEEDED(element->GetCurrentPropertyValue(id, &v)) &&
        v.vt == VT_BSTR) {
        out = v.bstrVal;
    }
    return out;
}

static void dump(IUIAutomation* automation, IUIAutomationElement* root,
                 const wchar_t* label, int* flameshotCount, int* total)
{
    if (root == nullptr) {
        return;
    }
    VARIANT none;
    VariantInit(&none);

    IUIAutomationCondition* cond = nullptr;
    if (FAILED(automation->CreateTrueCondition(&cond)) || cond == nullptr) {
        return;
    }
    IUIAutomationElementArray* children = nullptr;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond, &children)) ||
        children == nullptr) {
        cond->Release();
        return;
    }
    int n = 0;
    children->get_Length(&n);
    wprintf(L"--- %s: %d descendants\n", label, n);
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(children->GetElement(i, &element)) || element == nullptr) {
            continue;
        }
        BSTR name = getProp(element, UIA_NamePropertyId);
        BSTR automationId = getProp(element, UIA_AutomationIdPropertyId);
        int pid = 0;
        element->get_CurrentProcessId(&pid);
        RECT r{};
        element->get_CurrentBoundingRectangle(&r);
        const std::wstring title = name ? name : L"";
        if (!title.empty()) {
            wprintf(L"  [%4d] pid=%-6d %-28ls %-18ls %ls\n", i, pid,
                    title.c_str(),
                    automationId ? automationId : L"",
                    rectToString(r).c_str());
            ++(*total);
            if (title.find(L"Flameshot") != std::wstring::npos) {
                ++(*flameshotCount);
            }
        }
        if (name) {
            SysFreeString(name);
        }
        if (automationId) {
            SysFreeString(automationId);
        }
        element->Release();
    }
    children->Release();
    cond->Release();
}

int wmain()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        wprintf(L"CoInitializeEx failed\n");
        return 1;
    }
    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation),
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                __uuidof(IUIAutomation),
                                reinterpret_cast<void**>(&automation)))) {
        wprintf(L"CUIAutomation unavailable\n");
        return 2;
    }

    int flameshotCount = 0;
    int total = 0;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray != nullptr) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(automation->ElementFromHandle(tray, &root)) &&
            root != nullptr) {
            dump(automation, root, L"Shell_TrayWnd", &flameshotCount, &total);
            root->Release();
        }
    }
    HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    if (overflow != nullptr) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(automation->ElementFromHandle(overflow, &root)) &&
            root != nullptr) {
            dump(automation, root, L"Overflow", &flameshotCount, &total);
            root->Release();
        }
    }
    wprintf(L"\nTOTAL named elements=%d  flameshot icons=%d\n", total,
            flameshotCount);
    automation->Release();
    CoUninitialize();
    return 0;
}
