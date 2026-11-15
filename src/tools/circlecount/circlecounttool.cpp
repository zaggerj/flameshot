// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "circlecounttool.h"
#include "utils/colorutils.h"
#include "utils/confighandler.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace {
// Outline ring grows the bubble by PADDING_VALUE pixels in diameter.
constexpr int PADDING_VALUE = 2;
// Tool sizes 1..50 map to a bubble *diameter*. The floor is 18 px: at 5 px the
// digit was a single unreadable smudge, so "size 1" looked broken rather than
// small. 18 px keeps a two digit number legible on a 1080p screen and stays
// inside the smallest useful annotation.
//   size  1 -> 18 px      size  5 -> 24 px
//   size 15 -> 40 px     size 50 -> 97 px
constexpr int MIN_BUBBLE_DIAMETER = 18;
constexpr int DIAMETER_STEP_NUMERATOR = 8;
constexpr int DIAMETER_STEP_DENOMINATOR = 5;
// Below this diameter the digit is unreadable; draw a plain dot instead. With
// the floor above this only matters for future smaller mappings.
constexpr int NUMBER_MIN_DIAMETER = 14;
int const MIN_COUNTER = 1;
int const MAX_COUNTER = 999;

int bubbleDiameter(int toolSize)
{
    const int step = std::max(1, toolSize) - 1;
    return MIN_BUBBLE_DIAMETER +
           step * DIAMETER_STEP_NUMERATOR / DIAMETER_STEP_DENOMINATOR;
}

// Ceil of the radius, used for integer bounding rectangles.
int bubbleExtent(int toolSize)
{
    return (bubbleDiameter(toolSize) + 1) / 2;
}
}

CircleCountTool::CircleCountTool(QObject* parent)
  : AbstractTwoPointTool(parent)
  , m_valid(false)
{}

QIcon CircleCountTool::icon(const QColor& background, bool inEditor) const
{
    Q_UNUSED(inEditor)
    return QIcon(iconPath(background) + "circlecount-outline.svg");
}

QString CircleCountTool::info()
{
    m_tempString = QString("%1 - %2").arg(name(), count());
    return m_tempString;
}

bool CircleCountTool::isValid() const
{
    return m_valid;
}

QRect CircleCountTool::mousePreviewRect(const CaptureContext& context) const
{
    const int extent = bubbleDiameter(context.toolSize) + PADDING_VALUE * 2;
    QRect rect(0, 0, extent, extent);
    rect.moveCenter(context.mousePos);
    return rect;
}

QRect CircleCountTool::boundingRect() const
{
    if (!isValid()) {
        return {};
    }
    // Radius plus the outline ring. Must stay in sync with process(),
    // otherwise partial redraws leave artefacts behind.
    const int extent = bubbleExtent(size()) + PADDING_VALUE;

    int line_pos_min_x =
      std::min(points().first.x() - extent, points().second.x());
    int line_pos_min_y =
      std::min(points().first.y() - extent, points().second.y());
    int line_pos_max_x =
      std::max(points().first.x() + extent, points().second.x());
    int line_pos_max_y =
      std::max(points().first.y() + extent, points().second.y());

    return { line_pos_min_x,
             line_pos_min_y,
             line_pos_max_x - line_pos_min_x,
             line_pos_max_y - line_pos_min_y };
}

QString CircleCountTool::name() const
{
    return tr("Circle Counter");
}

CaptureTool::Type CircleCountTool::type() const
{
    return CaptureTool::TYPE_CIRCLECOUNT;
}

void CircleCountTool::copyParams(const CircleCountTool* from,
                                 CircleCountTool* to)
{
    AbstractTwoPointTool::copyParams(from, to);
    to->setCount(from->count());
    to->m_valid = from->m_valid;
}

QString CircleCountTool::description() const
{
    return tr("Add an autoincrementing counter bubble");
}

CaptureTool* CircleCountTool::copy(QObject* parent)
{
    auto* tool = new CircleCountTool(parent);
    copyParams(this, tool);
    return tool;
}

void CircleCountTool::process(QPainter& painter, const QPixmap& pixmap)
{
    Q_UNUSED(pixmap)
    // save current pen, brush, and font state
    auto orig_pen = painter.pen();
    auto orig_brush = painter.brush();
    auto orig_font = painter.font();

    QColor contrastColor =
      ColorUtils::colorIsDark(color()) ? Qt::white : Qt::black;
    QColor antiContrastColor =
      ColorUtils::colorIsDark(color()) ? Qt::black : Qt::white;

    const int diameter = bubbleDiameter(size());
    const qreal radius = diameter / 2.0;
    const QPointF center(points().first);
    // QPainter::drawEllipse(center, r, r) rasterises half-open boundaries,
    // which yields 4x4 px for a nominal 5 px circle. An integer-aligned rect
    // gives exactly `diameter` pixels and keeps tiny markers predictable.
    const int half = diameter / 2;
    const QRect bubbleRect(qRound(center.x()) - half,
                           qRound(center.y()) - half,
                           diameter,
                           diameter);
    // Qt's ellipse rasteriser uses a half-open bottom/right edge, so a
    // d x d rect yields only d-1 rows/cols. Extending by one pixel restores
    // the exact `diameter` x `diameter` footprint.
    const QRectF bubbleShape =
      diameter < NUMBER_MIN_DIAMETER
        ? QRectF(bubbleRect).adjusted(0, 0, 0, 1)
        : QRectF(bubbleRect).adjusted(0, 0, 1, 1);

    QLineF line(center, QPointF(points().second));
    // Pointer: only when the second point lies outside the bubble.
    if (line.length() > radius) {
        painter.setPen(QPen(color(), 0));
        painter.setBrush(color());

        QLineF normal = line.normalVector();
        normal.setLength(radius);
        QPoint p1 = normal.p2().toPoint();
        QPoint p2(qRound(center.x() - (p1.x() - center.x())),
                  qRound(center.y() - (p1.y() - center.y())));

        QPainterPath path;
        path.moveTo(center);
        path.lineTo(p1);
        path.lineTo(points().second);
        path.lineTo(p2);
        path.lineTo(center);
        painter.drawPath(path);
    }

    // The contrast ring exists to keep the digit readable. On tiny markers
    // it would grow the footprint beyond the requested 5x5 px, so draw only
    // the plain dot there.
    const bool showNumber = diameter >= NUMBER_MIN_DIAMETER;
    if (showNumber && ConfigHandler().drawCircleCounterOutline()) {
        painter.setPen(contrastColor);
        painter.setBrush(antiContrastColor);
        painter.drawEllipse(
          bubbleShape.adjusted(-PADDING_VALUE / 2.0,
                               -PADDING_VALUE / 2.0,
                               PADDING_VALUE / 2.0,
                               PADDING_VALUE / 2.0));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(color());
    painter.drawEllipse(bubbleShape);

    // A digit is illegible on tiny markers: draw a plain dot instead.
    if (showNumber) {
        QRect textRect = bubbleRect;
        auto new_font = orig_font;
        int fontSize = diameter;
        new_font.setPixelSize(fontSize);
        new_font.setBold(true);
        painter.setFont(new_font);

        QRect bRect = painter.boundingRect(
          textRect, Qt::AlignCenter, QString::number(count()));
        while (bRect.width() > textRect.width() && fontSize > 1) {
            --fontSize;
            new_font.setPixelSize(fontSize);
            painter.setFont(new_font);
            bRect = painter.boundingRect(
              textRect, Qt::AlignCenter, QString::number(count()));
        }

        painter.setPen(contrastColor);
        painter.drawText(textRect, Qt::AlignCenter, QString::number(count()));
    }

    // restore original font, brush, and pen
    painter.setFont(orig_font);
    painter.setBrush(orig_brush);
    painter.setPen(orig_pen);
}

void CircleCountTool::paintMousePreview(QPainter& painter,
                                        const CaptureContext& context)
{
    // drawStart() stores context.toolSize verbatim; do the same here so the
    // preview matches the marker that will actually be committed.
    onSizeChanged(context.toolSize);

    const int diameter = bubbleDiameter(size());
    painter.save();
    painter.setOpacity(0.35);
    // A round-capped one-pixel line with this pen width renders as a filled
    // circle of `diameter` pixels.
    painter.setPen(QPen(context.color, diameter, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(context.mousePos,
                     { context.mousePos.x() + 1, context.mousePos.y() + 1 });

    if (diameter >= NUMBER_MIN_DIAMETER) {
        auto font = painter.font();
        font.setPixelSize(diameter / 2);
        font.setBold(true);
        painter.setFont(font);

        painter.setPen(QPen("white"));
        painter.drawText(QRect{ context.mousePos.x() - diameter / 2,
                                context.mousePos.y() - diameter / 2,
                                diameter,
                                diameter },
                         Qt::AlignCenter,
                         QString::number(context.circleCount));
    }

    painter.restore();
}

bool CircleCountTool::handleMouseWheelEvent(int delta,
                                            bool adjustmentButtonPressed,
                                            CaptureContext& ctx)
{
    if (adjustmentButtonPressed) {
        ctx.circleCount =
          qBound(MIN_COUNTER, ctx.circleCount + delta, MAX_COUNTER);
    }
    return adjustmentButtonPressed;
}

void CircleCountTool::drawStart(const CaptureContext& context)
{
    setCount(context.circleCount);
    // ------------------------------------------------------------------
    // Temporary solution until the circle count variable removed from
    // the context altogether and made part of Circle count tool
    CaptureContext& ctx = const_cast<CaptureContext&>(context);
    ctx.circleCount = qBound(MIN_COUNTER, ctx.circleCount + 1, MAX_COUNTER);
    // ------------------------------------------------------------------
    AbstractTwoPointTool::drawStart(context);
    m_valid = true;
}

void CircleCountTool::pressed(CaptureContext& context)
{
    Q_UNUSED(context)
}
