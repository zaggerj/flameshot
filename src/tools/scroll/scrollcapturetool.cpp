// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors

#include "scrollcapturetool.h"

#include <QIcon>

ScrollCaptureTool::ScrollCaptureTool(QObject* parent)
  : AbstractActionTool(parent)
{}

bool ScrollCaptureTool::closeOnButtonPressed() const
{
    return true;
}

QIcon ScrollCaptureTool::icon(const QColor& background, bool inEditor) const
{
    Q_UNUSED(inEditor)
    return QIcon(iconPath(background) + "scroll-capture.svg");
}

QString ScrollCaptureTool::name() const
{
    return tr("Scrolling Capture");
}

QString ScrollCaptureTool::description() const
{
    return tr("Scroll a page and stitch it into one long image");
}

CaptureTool::Type ScrollCaptureTool::type() const
{
    return CaptureTool::TYPE_SCROLLCAPTURE;
}

CaptureTool* ScrollCaptureTool::copy(QObject* parent)
{
    return new ScrollCaptureTool(parent);
}

void ScrollCaptureTool::pressed(CaptureContext& context)
{
    Q_UNUSED(context)
    emit requestAction(REQ_SCROLL_CAPTURE);
}
