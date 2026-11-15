// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors

#pragma once

#include "tools/abstractactiontool.h"

// Toolbar entry point for the scrolling capture.
//
// The editor works on a frozen picture of the screen, so scrolling underneath
// it would never show up. Pressing this button therefore does not draw
// anything: it hands the current selection over to the scrolling capture,
// which closes the editor and works on the live desktop instead. See
// CaptureWidget::handleToolSignal(REQ_SCROLL_CAPTURE).
class ScrollCaptureTool : public AbstractActionTool
{
    Q_OBJECT
public:
    explicit ScrollCaptureTool(QObject* parent = nullptr);

    bool closeOnButtonPressed() const override;

    QIcon icon(const QColor& background, bool inEditor) const override;
    QString name() const override;
    QString description() const override;

    CaptureTool* copy(QObject* parent = nullptr) override;

protected:
    CaptureTool::Type type() const override;

public slots:
    void pressed(CaptureContext& context) override;
};
