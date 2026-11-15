// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors

#include "scrolluia.h"
#include "utils/abstractlogger.h"

#include <QStringList>
#include <qt_windows.h>

#include <uiautomationclient.h>
#include <uiautomationcore.h>

#ifdef _MSC_VER
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace {

// UIA_ScrollPatternNoScroll; mingw-w64 ships the interface but not this one
// constant, and its value is fixed by the UI Automation specification.
constexpr double kNoScroll = -1.0;

IUIAutomation* asUia(void* pointer)
{
    return static_cast<IUIAutomation*>(pointer);
}

IUIAutomationScrollPattern* asScroll(void* pointer)
{
    return static_cast<IUIAutomationScrollPattern*>(pointer);
}

QString controlTypeName(CONTROLTYPEID id)
{
    switch (id) {
        case UIA_DocumentControlTypeId:
            return QStringLiteral("Document");
        case UIA_PaneControlTypeId:
            return QStringLiteral("Pane");
        case UIA_WindowControlTypeId:
            return QStringLiteral("Window");
        case UIA_ListControlTypeId:
            return QStringLiteral("List");
        case UIA_TableControlTypeId:
            return QStringLiteral("Table");
        case UIA_EditControlTypeId:
            return QStringLiteral("Edit");
        case UIA_CustomControlTypeId:
            return QStringLiteral("Custom");
        case UIA_GroupControlTypeId:
            return QStringLiteral("Group");
        default:
            return QStringLiteral("Control(%1)").arg(static_cast<int>(id));
    }
}

} // namespace

ScrollUia::ScrollUia() = default;

ScrollUia::~ScrollUia()
{
    release();
    if (m_uia != nullptr) {
        asUia(m_uia)->Release();
        m_uia = nullptr;
    }
}

void ScrollUia::release()
{
    if (m_scroll != nullptr) {
        asScroll(m_scroll)->Release();
        m_scroll = nullptr;
    }
    m_description.clear();
    m_bounds = QRect();
}

bool ScrollUia::attach(const QPoint& physical)
{
    release();

    if (m_uia == nullptr) {
        // The GUI thread is already an STA; a second initialization is a
        // harmless no-op (S_FALSE) and RPC_E_CHANGED_MODE means somebody else
        // picked MTA first, which still lets us talk to UI Automation.
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IUIAutomation* uia = nullptr;
        const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation,
                                            nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_IUIAutomation,
                                            reinterpret_cast<void**>(&uia));
        if (FAILED(hr) || uia == nullptr) {
            AbstractLogger::warning(AbstractLogger::LogFile)
              << QStringLiteral("UI Automation unavailable (CoCreateInstance "
                                "hr=0x%1, init=0x%2)")
                   .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
                   .arg(static_cast<quint32>(init), 8, 16, QLatin1Char('0'));
            if (SUCCEEDED(init)) {
                CoUninitialize();
            }
            return false;
        }
        m_uia = uia;
    }

    POINT point{ physical.x(), physical.y() };
    IUIAutomationElement* element = nullptr;
    const HRESULT hr =
      asUia(m_uia)->ElementFromPoint(point, &element);
    if (FAILED(hr) || element == nullptr) {
        return false;
    }

    IUIAutomationTreeWalker* walker = nullptr;
    asUia(m_uia)->get_ControlViewWalker(&walker);

    IUIAutomationElement* current = element;
    bool found = false;
    // Deep enough for iframe -> document -> pane -> window, shallow enough to
    // stop before climbing into the whole desktop.
    for (int depth = 0; current != nullptr && depth < 14; ++depth) {
        CONTROLTYPEID type = 0;
        current->get_CurrentControlType(&type);

        BSTR name = nullptr;
        current->get_CurrentName(&name);
        const QString elementName =
          name != nullptr ? QString::fromWCharArray(name) : QString();
        if (name != nullptr) {
            SysFreeString(name);
        }

        IUIAutomationScrollPattern* pattern = nullptr;
        if (SUCCEEDED(current->GetCurrentPatternAs(
              UIA_ScrollPatternId,
              IID_IUIAutomationScrollPattern,
              reinterpret_cast<void**>(&pattern))) &&
            pattern != nullptr) {
            double viewSize = -1.0;
            pattern->get_CurrentVerticalViewSize(&viewSize);
            // A container that shows all of its content reports 100% and
            // cannot scroll; keep walking, the real viewport is usually the
            // parent of such an element.
            if (viewSize > 0.0 && viewSize < 99.5) {
                double position = -1.0;
                pattern->get_CurrentVerticalScrollPercent(&position);
                RECT bounds{};
                if (SUCCEEDED(current->get_CurrentBoundingRectangle(&bounds))) {
                    m_bounds = QRect(QPoint(static_cast<int>(bounds.left),
                                            static_cast<int>(bounds.top)),
                                     QPoint(static_cast<int>(bounds.right) - 1,
                                            static_cast<int>(bounds.bottom) - 1));
                }
                m_scroll = pattern;
                m_description = QStringLiteral("%1 \"%2\" (view %3%)")
                                  .arg(controlTypeName(type),
                                       elementName.left(60),
                                       QString::number(viewSize, 'f', 1));
                found = true;
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral(
                       "UI Automation scroll container found at depth %1: %2, "
                       "position=%3%")
                       .arg(depth)
                       .arg(m_description)
                       .arg(position, 0, 'f', 1);
                break;
            }
            // A container that reports its full content as visible cannot
            // scroll; keep walking upwards, the real viewport is usually its
            // parent.
            pattern->Release();
            pattern = nullptr;
        }

        if (walker == nullptr) {
            break;
        }
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(current, &parent)) ||
            parent == nullptr) {
            break;
        }
        if (current != element) {
            current->Release();
        }
        current = parent;
    }

    if (current != nullptr && current != element) {
        current->Release();
    }
    element->Release();
    if (walker != nullptr) {
        walker->Release();
    }

    if (!found) {
        // The caller samples several points and reports the aggregate; logging
        // every miss here would bury the useful lines.
        return false;
    }
    return true;
}

double ScrollUia::verticalPercent() const
{
    if (m_scroll == nullptr) {
        return -1.0;
    }
    double value = -1.0;
    if (FAILED(asScroll(m_scroll)->get_CurrentVerticalScrollPercent(&value))) {
        return -1.0;
    }
    return value;
}

double ScrollUia::viewPercent() const
{
    if (m_scroll == nullptr) {
        return -1.0;
    }
    double value = -1.0;
    if (FAILED(asScroll(m_scroll)->get_CurrentVerticalViewSize(&value))) {
        return -1.0;
    }
    return value;
}

QRect ScrollUia::boundsPhysical() const
{
    return m_bounds;
}

bool ScrollUia::scrollToPercent(double percent)
{
    if (m_scroll == nullptr) {
        return false;
    }
    const double clamped = qBound(0.0, percent, 100.0);
    return SUCCEEDED(
      asScroll(m_scroll)->SetScrollPercent(kNoScroll, clamped));
}

bool ScrollUia::scrollByPage()
{
    if (m_scroll == nullptr) {
        return false;
    }
    return SUCCEEDED(asScroll(m_scroll)->Scroll(ScrollAmount_NoAmount,
                                                ScrollAmount_LargeIncrement));
}
