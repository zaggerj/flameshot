// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors
//
// UI Automation bridge for scrolling capture.
//
// Blindly injecting a wheel event into whatever happens to sit under the
// middle of the selection is not the same thing as finding the scrollable
// area: the middle can be a margin, a fixed pane, or the desktop wallpaper,
// and then nothing moves. Windows exposes the real answer through UI
// Automation - every scrollable container implements ScrollPattern, which
// reports the current position and the visible fraction of the content and
// can be scrolled without stealing the pointer or the keyboard focus.
//
// This wrapper is deliberately small: attach to a point, walk the parent
// chain until a ScrollPattern appears, then read/drive it.

#pragma once

#include <QPoint>
#include <QRect>
#include <QString>

class ScrollUia
{
public:
    ScrollUia();
    ~ScrollUia();
    ScrollUia(const ScrollUia&) = delete;
    ScrollUia& operator=(const ScrollUia&) = delete;

    /// Finds the innermost scrollable container at \a physical (virtual
    /// desktop pixels). Returns false when the point is not inside one.
    bool attach(const QPoint& physical);

    bool attached() const { return m_scroll != nullptr; }

    /// Human readable container description, for the log file.
    const QString& description() const { return m_description; }

    /// Vertical scroll position, 0 = top, 100 = bottom. Negative when the
    /// provider does not report it.
    double verticalPercent() const;
    /// Visible fraction of the scrollable content, in percent. Negative when
    /// unknown.
    double viewPercent() const;

    /// Screen rectangle of the container, in physical virtual-desktop pixels.
    /// Empty when the provider does not report it.
    QRect boundsPhysical() const;

    /// Absolute positioning. Providers that only implement ScrollPattern
    /// incrementally return false.
    bool scrollToPercent(double percent);

    /// One large increment (roughly a page).
    bool scrollByPage();

    void release();

private:
    void* m_uia = nullptr;      // IUIAutomation*
    void* m_scroll = nullptr;   // IUIAutomationScrollPattern*
    QRect m_bounds;
    QString m_description;
};
