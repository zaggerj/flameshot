// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors

#pragma once

#include <QObject>

// Windows-only stage-0 scrolling capture.
//
// Flameshot freezes a single QPixmap in a fullscreen overlay, so scrolling has
// to happen *before* CaptureWidget is created. This class selects a region,
// scrolls the window underneath it, stitches the frames and exports the long
// image straight to the clipboard and to a PNG file. The annotation pipeline
// is deliberately not involved (v1).
class ScrollCapture : public QObject
{
    Q_OBJECT
public:
    static ScrollCapture* instance();

    // Synchronous: runs nested event loops while selecting and capturing.
    // Safe to call from a global hotkey or from the CLI.
    void start();

    // Same, but reusing a region the caller already picked (the editor's
    // toolbar button hands its selection over). `logicalRegion` is in global
    // logical coordinates; an empty rectangle falls back to the selector.
    void start(const QRect& logicalRegion);

    /// True while the region selector or the scrolling session is running.
    bool isActive() const { return m_running; }

    /// Diagnostic entry point (`flameshot scroll-probe`). For every visible
    /// top-level window it reports which window would be picked as the scroll
    /// target and whether a scrollable UI Automation container lives inside
    /// it. Read-only: nothing is scrolled, no foreground is taken.
    static void diagnose();

#ifdef ENABLE_SELFTEST
    /// Acceptance test for the region confirmation overlay. Builds it
    /// offscreen (no window is shown, no pixel of the desktop is read), drives
    /// it with key events and renders it to check that the frame, handles and
    /// the confirm button are actually painted.
    static bool pickerSelfTest(QString& report);
#endif

signals:
    void finished(bool ok, const QString& detail);

private:
    explicit ScrollCapture(QObject* parent = nullptr);
    Q_DISABLE_COPY(ScrollCapture)

    bool m_running = false;
};
