// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot contributors
//
// Scrolling capture for Windows.
//
// The stitching core follows the approach shared by every mature scrolling
// capture implementation (ShareX, deepin-screen-recorder, wayscrollshot,
// Picsew, CaptureOCR): never run a full resolution 2D search. Compress each
// frame to a row profile, drop the parts that do not move (static sidebars,
// scrollbars, fixed header and footer bars), then search the single vertical
// translation on the remaining columns, starting from the predicted step and
// expanding outwards. See docs: 滚动截屏调研.md.

#include "scrollcapture.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"
#include "utils/scrolluia.h"
#include "utils/windowforeground.h"
#include "utils/zaggertr.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>

#include <qt_windows.h>

namespace {

constexpr int MIN_REGION_SIZE = 40;
constexpr int MAX_RESULT_HEIGHT = 32768;
constexpr int MAX_FRAMES = 400;
constexpr int NOTCH_DELTA = 100;   // logical px an application scrolls/notch
constexpr int SETTLE_TIMEOUT_MS = 2600;
constexpr int SETTLE_POLL_MS = 60;
// A smooth scroll animation has to be over before a frame can be used. Rather
// than waiting a fixed second (which is both too slow for static pages and too
// short for long animations), require the frame to stay identical for a
// continuous span.
constexpr int MIN_SETTLE_MS = 150;
constexpr int STABLE_SPAN_MS = 240;
constexpr int MIN_SHIFT = 2;
constexpr int MODE_COUNT = 3;

// --- row profile -----------------------------------------------------------

// Columns are spread over the frame, skipping the left 2% and the right 8%
// (the scrollbar redraws on every step and would poison the match).
constexpr int PROFILE_COLUMNS = 48;
constexpr int LEFT_MARGIN_PERCENT = 2;
constexpr int RIGHT_MARGIN_PERCENT = 8;

// A column whose content does not change between two frames is a static
// sidebar (or a flat margin). It matches at every offset, so it must be
// removed before searching; otherwise every candidate scores equally well and
// the search locks onto 0.
constexpr int COLUMN_ACTIVITY_MIN = 2;
constexpr int COLUMN_FLAT_VARIANCE = 3;
constexpr int MIN_USABLE_COLUMNS = 8;
constexpr int FALLBACK_COLUMNS = 12;

// Fixed header / footer: rows that are byte identical in both frames. Capped
// at 35% of the frame so an entirely static page is not mistaken for one big
// fixed bar.
constexpr int FIXED_BAR_MAX_PERCENT = 35;
constexpr int FIXED_BAR_MIN_ROWS = 4;
constexpr double FIXED_ROW_DELTA = 1.5;

// Mean absolute greyscale difference thresholds. 8bit grey, so 2.5 is "the
// same pixels with sub-pixel noise", 24 is "clearly different content".
constexpr double MAD_PERFECT = 2.5;
constexpr double MAD_GOOD = 8.0;
constexpr double MAD_ACCEPT = 24.0;
constexpr double MAD_IDENTICAL = 0.8;
// A candidate is only trustworthy when no other distinct offset comes close:
// repeating cards and flat areas both produce several near-equal minima.
constexpr double AMBIGUITY_RATIO = 0.75;
constexpr int AMBIGUITY_MIN_GAP = 8;
constexpr int SCORE_SAMPLED_ROWS = 300;

// Keep each step well below the region height, otherwise consecutive frames
// share no overlap and the vertical shift cannot be measured.
int notchesForRegion(int logicalHeight)
{
    const int target = qMax(NOTCH_DELTA, qRound(logicalHeight * 0.4));
    return qBound(1, target / NOTCH_DELTA, 5);
}

qint64 g_grabCount = 0;
qint64 g_grabMs = 0;

bool envFlag(const char* name, bool fallback)
{
    const QByteArray value = qgetenv(name);
    if (value.isEmpty()) {
        return fallback;
    }
    return value != "0" && value.compare("false", Qt::CaseInsensitive) != 0;
}

// Fullscreen translucent overlay used only to pick the scroll region. It is
// destroyed before the first frame is grabbed, so it never appears in the
// resulting image.
class RegionSelector : public QDialog
{
public:
    explicit RegionSelector(QWidget* parent = nullptr)
      : QDialog(parent,
                Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::CrossCursor);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        // Cover the ENTIRE virtual desktop, not just the monitor under the
        // mouse cursor. The old single-screen overlay meant the user could
        // only select on the monitor the mouse happened to be on; dragging on
        // the other monitor did nothing, which looked exactly like "the
        // interaction disappeared" (2026-11-15 user report).
        const QRect virtualDesktop = QGuiApplication::primaryScreen()
                                       ? QGuiApplication::primaryScreen()->virtualGeometry()
                                       : QRect(0, 0, 1920, 1080);
        setGeometry(virtualDesktop);
        // The screen used for DPI / grab scaling is determined AFTER the user
        // picks a region (see screen() below), not from the cursor position.
        QGuiAppCurrentScreen screenFinder;
        m_screen = screenFinder.currentScreen();
        // Tool windows do not always receive keyboard focus on Windows, and a
        // capture started from a global hotkey never owns the foreground.
        // Esc must cancel the selection, so force the focus.
        QTimer::singleShot(0, this, [this]() {
            FlameshotWindow::forceForeground(this);
            setFocus();
        });
    }

    // m_rect is in selector-local coordinates; the selector covers the whole
    // virtual desktop, so translate to global logical coordinates.
    QRect selected() const { return m_rect.translated(geometry().topLeft()); }
    // The screen that matters is the one the selection is ON, not the one the
    // mouse cursor happens to be over.
    QScreen* screen() const
    {
        if (!m_rect.isNull()) {
            const QPoint center =
              m_rect.translated(geometry().topLeft()).center();
            for (QScreen* s : QGuiApplication::screens()) {
                if (s->geometry().contains(center)) {
                    return s;
                }
            }
        }
        return m_screen;
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_origin = event->position().toPoint();
        m_rect = QRect(m_origin, QSize());
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            return;
        }
        m_rect = QRect(m_origin, event->position().toPoint()).normalized();
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        Q_UNUSED(event)
        if (m_rect.width() >= MIN_REGION_SIZE &&
            m_rect.height() >= MIN_REGION_SIZE) {
            accept();
        } else {
            m_rect = QRect();
            update();
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            reject();
        }
    }

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 90));
        if (m_rect.isNull()) {
            return;
        }
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(m_rect, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setPen(QPen(Qt::red, 2));
        painter.drawRect(m_rect);
    }

private:
    QPoint m_origin;
    QRect m_rect;
    QScreen* m_screen = nullptr;
};

// Progress indicator. A capture of a long page runs for up to a minute with
// no visible change, which reads as a hang. The widget is click-through, never
// takes focus (that would move the wheel target) and is placed outside the
// selected region, so it cannot end up in the stitched image.
class CaptureHud : public QWidget
{
public:
    CaptureHud(const QRect& region, QScreen* screen)
      : QWidget(nullptr,
                Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                  Qt::WindowTransparentForInput |
                  Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(340, 74);

        const QRect area = screen->geometry();
        QPoint topRight(area.right() - width() - 24, area.top() + 24);
        QPoint bottomLeft(area.left() + 24, area.bottom() - height() - 24);
        const QRect candidate(topRight, size());
        const QPoint origin =
          candidate.intersects(region) ? bottomLeft : topRight;
        move(origin);

        show();
        raise();
        QCoreApplication::processEvents();
    }

    void setStatus(const QString& text)
    {
        m_text = text;
        update();
    }

    void setTitle(const QString& text)
    {
        m_title = text;
        update();
    }

    // ShareX reports capture quality with a green / yellow / red indicator.
    // The same three states, appended to the title row: green = every seam
    // matched with high confidence, yellow = some seams needed a best guess,
    // red = the capture gave up and delivered the static frame.
    void setBadge(const QColor& color)
    {
        m_badge = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 20, 24, 225));
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);
        painter.setPen(QColor(255, 255, 255));
        QFont title = font();
        title.setPointSizeF(title.pointSizeF() + 1.5);
        title.setBold(true);
        painter.setFont(title);
        painter.drawText(QRect(16, 12, width() - 60, 24),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         m_title.isEmpty() ? ZAG_TR("Scrolling capture…")
                                           : m_title);
        if (m_badge.isValid()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_badge);
            painter.drawEllipse(QRect(width() - 34, 15, 16, 16));
        }
        painter.setPen(QColor(170, 176, 190));
        painter.setFont(font());
        painter.drawText(QRect(16, 38, width() - 32, 24),
                         Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }

private:
    QString m_text;
    QString m_title;
    QColor m_badge;
};

// ---------------------------------------------------------------------------
// Confirmation step: "which rectangle should scroll?"
// ---------------------------------------------------------------------------
//
// A scrolling capture used to start the instant the toolbar button was
// pressed: the editor selection was taken as-is and the wheel started to turn.
// When the selection covered a sidebar, a tab bar or the wallpaper, the result
// was a stitched image of something that never moved, with no chance to notice
// before the fact.
//
// This overlay makes the target explicit. It shows the rectangle that will be
// scrolled, snaps the suggestion to the scroll container UI Automation found,
// and lets the user drag or resize it. Nothing scrolls until Enter or the
// "开始滚动" button - the "select, confirm, shoot" order every mature
// scrolling capture uses (ShareX, Snipaste, the Windows Snipping Tool).
class ScrollRegionPicker : public QWidget
{
public:
    enum Handle
    {
        None,
        Move,
        NewSelection,
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left
    };

    ScrollRegionPicker(QScreen* screen,
                       const QRect& initial,
                       const QRect& detected,
                       const QString& detectedLabel,
                       const QPixmap& background = QPixmap())
      : QWidget(nullptr,
                Qt::Tool | Qt::FramelessWindowHint |
                  Qt::WindowStaysOnTopHint)
      , m_screen(screen)
      , m_detectedLabel(detectedLabel)
      , m_handle(None)
    {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        setWindowTitle(ZAG_TR("Scrolling capture"));
        setGeometry(screen->geometry());
        m_origin = screen->geometry().topLeft();
        m_frame = QRect(QPoint(0, 0), size());

        m_background = background;
        if (m_background.isNull()) {
            m_background = screen->grabWindow(0);
        }

        m_detected = detected.normalized().translated(-m_origin);
        if (m_detected.width() < MIN_REGION_SIZE ||
            m_detected.height() < MIN_REGION_SIZE) {
            m_detected = QRect();
        }

        m_region = initial.normalized().translated(-m_origin);
        if (m_region.width() < MIN_REGION_SIZE ||
            m_region.height() < MIN_REGION_SIZE) {
            m_region = m_detected.isValid()
                         ? m_detected
                         : QRect(m_frame.center() - QPoint(320, 240),
                                 QSize(640, 480));
        }
        m_region = clampRegion(m_region);
    }

    /// Region the user confirmed, in global logical coordinates.
    QRect selected() const { return m_region.translated(m_origin); }

    /// Test hook: true once Enter / the confirm button was used.
    bool confirmed() const { return m_confirmed; }

    bool exec()
    {
        show();
        raise();
        activateWindow();
        setFocus(Qt::OtherFocusReason);
        // Qt::Tool windows do not reliably win the Windows foreground, and
        // after the drag the foreground belongs to the target application:
        // without this, the user's Enter / Esc go somewhere else entirely.
        QTimer::singleShot(0, this, [this]() {
            FlameshotWindow::forceForeground(this);
            setFocus();
        });
        QEventLoop loop;
        m_loop = &loop;
        loop.exec();
        m_loop = nullptr;
        hide();
        return m_confirmed;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (!m_background.isNull()) {
            p.drawPixmap(rect(), m_background);
        } else {
            p.fillRect(rect(), QColor(24, 26, 32));
        }
        // Dim everything, then punch the selection back to full brightness so
        // the user sees exactly what will be captured.
        p.fillRect(rect(), QColor(8, 10, 14, 150));
        if (m_region.isValid() && !m_background.isNull()) {
            const qreal sx = qreal(m_background.width()) / qMax(1, width());
            const qreal sy = qreal(m_background.height()) / qMax(1, height());
            const QRect src(qRound(m_region.x() * sx),
                            qRound(m_region.y() * sy),
                            qRound(m_region.width() * sx),
                            qRound(m_region.height() * sy));
            p.drawPixmap(m_region, m_background, src);
        }

        // Detected scroll container: a hint, not a constraint. When the
        // selection was snapped to it, say so: the user should know that
        // pressing Enter right away is the intended fast path.
        if (m_detected.isValid()) {
            QPen dashed(QColor(34, 197, 94, 230), 1, Qt::DashLine);
            p.setPen(dashed);
            p.setBrush(Qt::NoBrush);
            p.drawRect(m_detected.adjusted(0, 0, -1, -1));
            const bool snapped = m_region == m_detected;
            QString tag;
            if (snapped) {
                tag = m_detectedLabel.isEmpty()
                        ? ZAG_TR("Snapped to scroll container · Enter to start, drag to adjust")
                        : ZAG_TR("Snapped: %1 · Enter to start, drag to adjust")
                              .arg(m_detectedLabel);
            } else {
                tag = m_detectedLabel.isEmpty()
                        ? ZAG_TR("Scrollable area detected")
                        : ZAG_TR("Detected: %1").arg(m_detectedLabel);
            }
            drawChip(p,
                     QPoint(m_detected.left(),
                            qMax(4, m_detected.top() - 26)),
                     tag,
                     QColor(34, 197, 94, 230));
        }

        // Selection frame.
        p.setPen(QPen(QColor(56, 189, 248), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(m_region.adjusted(0, 0, -1, -1));

        // Resize handles.
        p.setPen(QPen(QColor(56, 189, 248), 1));
        p.setBrush(QColor(255, 255, 255, 235));
        for (const QRect& handle : handleRects()) {
            p.drawRect(handle.adjusted(0, 0, -1, -1));
        }

        // Size chip above the frame, title chip at the top of the screen.
        drawChip(p,
                 QPoint(m_region.left(),
                        qMax(4, m_region.top() - 26)),
                 QStringLiteral("%1 × %2")
                   .arg(m_region.width())
                   .arg(m_region.height()),
                 QColor(56, 189, 248));

        const QString headline =
          m_detected.isValid()
            ? ZAG_TR("Scrolling capture: adjust the content area, then start")
            : ZAG_TR("Scrolling capture: no container found, will use wheel scrolling");
        QFont title = font();
        title.setPointSizeF(title.pointSizeF() + 1.5);
        title.setBold(true);
        p.setFont(title);
        const QRect headlineRect(24, 20, width() - 48, 30);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(12, 14, 18, 200));
        p.drawRoundedRect(headlineRect.adjusted(-10, -6, 10, 6), 8, 8);
        p.setPen(QColor(m_detected.isValid() ? QColor(134, 239, 172)
                                             : QColor(253, 224, 71)));
        p.drawText(headlineRect, Qt::AlignLeft | Qt::AlignVCenter, headline);

        // Hint bar with the confirm button.
        p.setFont(font());
        const QRect bar = hintBarRect();
        p.setPen(QPen(QColor(255, 255, 255, 36), 1));
        p.setBrush(QColor(18, 20, 26, 240));
        p.drawRoundedRect(bar, 12, 12);
        p.setPen(QColor(203, 213, 225));
        p.drawText(bar.adjusted(18, 0, -140, 0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   ZAG_TR(
                     "Drag to adjust · Arrow keys to nudge · Enter to start · Esc to cancel"));

        const QRect go = goButtonRect();
        p.setPen(Qt::NoPen);
        p.setBrush(m_goHover ? QColor(37, 99, 235) : QColor(29, 78, 216));
        p.drawRoundedRect(go, 8, 8);
        p.setPen(Qt::white);
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.drawText(go, Qt::AlignCenter, ZAG_TR("Start scrolling"));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) {
            return;
        }
        const QPoint pos = e->pos();
        if (goButtonRect().contains(pos)) {
            confirm();
            return;
        }
        m_handle = handleAt(pos);
        m_pressPos = pos;
        m_pressRegion = m_region;
        if (m_handle == None) {
            // Dragging outside the current frame starts a new one.
            m_handle = NewSelection;
            m_region = QRect(pos, QSize(1, 1));
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPoint pos = e->pos();
        const bool goHover = goButtonRect().contains(pos);
        if (goHover != m_goHover) {
            m_goHover = goHover;
            update();
        }
        if (!(e->buttons() & Qt::LeftButton)) {
            setCursor(cursorFor(handleAt(pos)));
            return;
        }
        applyDrag(pos);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        Q_UNUSED(e)
        m_handle = None;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (m_region.contains(e->pos())) {
            confirm();
        }
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
        const bool resize = e->modifiers() & Qt::ControlModifier;
        switch (e->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                confirm();
                return;
            case Qt::Key_Escape:
                m_confirmed = false;
                if (m_loop != nullptr) {
                    m_loop->quit();
                }
                return;
            case Qt::Key_Left:
                nudge(-step, 0, resize);
                return;
            case Qt::Key_Right:
                nudge(step, 0, resize);
                return;
            case Qt::Key_Up:
                nudge(0, -step, resize);
                return;
            case Qt::Key_Down:
                nudge(0, step, resize);
                return;
            default:
                break;
        }
        QWidget::keyPressEvent(e);
    }

private:
    QRect clampRegion(const QRect& region) const
    {
        QRect out = region.normalized();
        out.setWidth(qBound(MIN_REGION_SIZE, out.width(), m_frame.width()));
        out.setHeight(qBound(MIN_REGION_SIZE, out.height(), m_frame.height()));
        if (out.left() < 0) {
            out.moveLeft(0);
        }
        if (out.top() < 0) {
            out.moveTop(0);
        }
        if (out.right() > m_frame.right()) {
            out.moveRight(m_frame.right());
        }
        if (out.bottom() > m_frame.bottom()) {
            out.moveBottom(m_frame.bottom());
        }
        return out;
    }

    QVector<QRect> handleRects() const
    {
        const int half = 5;
        const QPoint corners[] = {
            m_region.topLeft(),
            QPoint(m_region.center().x(), m_region.top()),
            m_region.topRight(),
            QPoint(m_region.right(), m_region.center().y()),
            m_region.bottomRight(),
            QPoint(m_region.center().x(), m_region.bottom()),
            m_region.bottomLeft(),
            QPoint(m_region.left(), m_region.center().y()),
        };
        QVector<QRect> rects;
        for (const QPoint& corner : corners) {
            rects << QRect(corner - QPoint(half, half), QSize(2 * half + 1,
                                                              2 * half + 1));
        }
        return rects;
    }

    Handle handleAt(const QPoint& pos) const
    {
        const Handle order[] = { TopLeft,  Top,    TopRight,  Right,
                                 BottomRight, Bottom, BottomLeft, Left };
        const QVector<QRect> rects = handleRects();
        for (int i = 0; i < rects.size() && i < 8; ++i) {
            if (rects.at(i).adjusted(-3, -3, 3, 3).contains(pos)) {
                return order[i];
            }
        }
        if (m_region.contains(pos)) {
            return Move;
        }
        return None;
    }

    static Qt::CursorShape cursorFor(Handle handle)
    {
        switch (handle) {
            case TopLeft:
            case BottomRight:
                return Qt::SizeFDiagCursor;
            case TopRight:
            case BottomLeft:
                return Qt::SizeBDiagCursor;
            case Top:
            case Bottom:
                return Qt::SizeVerCursor;
            case Left:
            case Right:
                return Qt::SizeHorCursor;
            case Move:
                return Qt::SizeAllCursor;
            default:
                return Qt::CrossCursor;
        }
    }

    void applyDrag(const QPoint& pos)
    {
        const QPoint delta = pos - m_pressPos;
        QRect r = m_pressRegion;
        switch (m_handle) {
            case Move:
                r.translate(delta);
                break;
            case NewSelection:
                r = QRect(m_pressPos, pos).normalized();
                break;
            case TopLeft:
                r.setTopLeft(m_pressRegion.topLeft() + delta);
                break;
            case Top:
                r.setTop(m_pressRegion.top() + delta.y());
                break;
            case TopRight:
                r.setTopRight(m_pressRegion.topRight() + delta);
                break;
            case Right:
                r.setRight(m_pressRegion.right() + delta.x());
                break;
            case BottomRight:
                r.setBottomRight(m_pressRegion.bottomRight() + delta);
                break;
            case Bottom:
                r.setBottom(m_pressRegion.bottom() + delta.y());
                break;
            case BottomLeft:
                r.setBottomLeft(m_pressRegion.bottomLeft() + delta);
                break;
            case Left:
                r.setLeft(m_pressRegion.left() + delta.x());
                break;
            default:
                return;
        }
        m_region = clampRegion(r);
        update();
    }

    void nudge(int dx, int dy, bool resize)
    {
        QRect r = m_region;
        if (resize) {
            r.setWidth(qMax(MIN_REGION_SIZE, r.width() + dx));
            r.setHeight(qMax(MIN_REGION_SIZE, r.height() + dy));
        } else {
            r.translate(dx, dy);
        }
        m_region = clampRegion(r);
        update();
    }

    void confirm()
    {
        if (m_region.width() < MIN_REGION_SIZE ||
            m_region.height() < MIN_REGION_SIZE) {
            return;
        }
        m_confirmed = true;
        if (m_loop != nullptr) {
            m_loop->quit();
        }
    }

    QRect hintBarRect() const
    {
        const int w = qMin(620, width() - 48);
        return QRect((width() - w) / 2, height() - 96, w, 52);
    }

    QRect goButtonRect() const
    {
        const QRect bar = hintBarRect();
        return QRect(bar.right() - 124, bar.top() + 9, 112, 34);
    }

    static void drawChip(QPainter& p,
                         const QPoint& origin,
                         const QString& text,
                         const QColor& color)
    {
        const QFontMetrics metrics(p.font());
        const QRect box(origin, QSize(metrics.horizontalAdvance(text) + 16, 22));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(12, 14, 18, 220));
        p.drawRoundedRect(box, 5, 5);
        p.setPen(color);
        p.drawText(box, Qt::AlignCenter, text);
    }

    QScreen* m_screen = nullptr;
    QPixmap m_background;
    QRect m_frame;
    QPoint m_origin;
    QRect m_region;
    QRect m_detected;
    QString m_detectedLabel;
    Handle m_handle = None;
    QPoint m_pressPos;
    QRect m_pressRegion;
    bool m_confirmed = false;
    bool m_goHover = false;
    QEventLoop* m_loop = nullptr;
};

// ShareX keeps a visible frame around the active capture area for the whole
// run (ScrollingCaptureRegionForm, WS_EX_TRANSPARENT + a Magenta "hole").
// Without it the user cannot tell whether the right content is being scrolled
// until the result image appears. This is the same idea: a click-through,
// focusless border drawn strictly OUTSIDE the captured rectangle, so no pixel
// of it can end up in the stitched image.
class RegionOutline : public QWidget
{
public:
    RegionOutline(QScreen* screen, const QRect& region)
      : QWidget(nullptr,
                Qt::Tool | Qt::FramelessWindowHint |
                  Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                  Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setGeometry(screen->geometry());
        m_region = region.translated(-screen->geometry().topLeft());
        show();
        raise();
        QCoreApplication::processEvents();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setBrush(Qt::NoBrush);
        // 3px band, 3px away from the captured rectangle: blitRegion reads
        // exactly the region rectangle, so anything outside it is free.
        const QPen pen(QColor(56, 189, 248, 210), 3);
        p.setPen(pen);
        const int g = 5;
        p.drawRect(m_region.adjusted(-g, -g, g, g));
    }

private:
    QRect m_region;
};

void pause(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}QRect physicalRect(QScreen* screen, const QRect& logicalRegion)
{
    const qreal dpr = screen->devicePixelRatio();
    const QPoint origin = screen->geometry().topLeft();
    return QRect(qRound((logicalRegion.x() - origin.x()) * dpr),
                 qRound((logicalRegion.y() - origin.y()) * dpr),
                 qRound(logicalRegion.width() * dpr),
                 qRound(logicalRegion.height() * dpr));
}

// QScreen::grabWindow blits the whole desktop and crops afterwards, which
// costs ~100ms per call on a 4K screen. The settle loop needs a dozen frames
// per scroll step, so the capture spends most of its wall clock inside the
// grabber. Blitting exactly the requested rectangle takes ~10ms.
QImage blitRegion(const QRect& physical, const QString& deviceName)
{
    if (physical.width() <= 0 || physical.height() <= 0) {
        return {};
    }

    // A per-monitor DC keeps the coordinates local to that monitor, so the
    // same code works on the primary screen and on a second screen that sits
    // above, below or to the left of it. The desktop DC (GetDC(nullptr))
    // would need virtual-desktop coordinates, which differ per monitor and
    // per DPI.
    HDC screenDC = nullptr;
    bool ownsDC = false;
    if (!deviceName.isEmpty()) {
        screenDC = CreateDCW(L"DISPLAY",
                             reinterpret_cast<LPCWSTR>(deviceName.utf16()),
                             nullptr,
                             nullptr);
        ownsDC = screenDC != nullptr;
    }
    if (screenDC == nullptr) {
        screenDC = GetDC(nullptr);
        ownsDC = false;
    }
    if (screenDC == nullptr) {
        return {};
    }
    auto releaseDC = [&]() {
        if (ownsDC) {
            DeleteDC(screenDC);
        } else {
            ReleaseDC(nullptr, screenDC);
        }
    };

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = physical.width();
    info.bmiHeader.biHeight = -physical.height(); // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    // A DIB section lets BitBlt write straight into memory we can read, while
    // CreateCompatibleBitmap + GetDIBits has to walk the bitmap again.
    HBITMAP bitmap = CreateDIBSection(
      screenDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memDC = CreateCompatibleDC(screenDC);
    if (bitmap != nullptr && memDC != nullptr && bits != nullptr) {
        HGDIOBJ previous = SelectObject(memDC, bitmap);
        // No CAPTUREBLT: including layered windows forces a full composition
        // pass and makes every grab noticeably slower.
        const BOOL blitted =
          BitBlt(memDC, 0, 0, physical.width(), physical.height(), screenDC,
                 physical.x(), physical.y(), SRCCOPY);
        SelectObject(memDC, previous);
        if (blitted) {
            const QImage view(static_cast<uchar*>(bits), physical.width(),
                              physical.height(), QImage::Format_RGB32);
            QImage copy = view.copy();
            copy.setDevicePixelRatio(1.0);
            DeleteObject(bitmap);
            DeleteDC(memDC);
            releaseDC();
            return copy;
        }
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memDC != nullptr) {
        DeleteDC(memDC);
    }
    releaseDC();
    return QImage();
}

QImage grabFrame(QScreen* screen, const QRect& logicalRegion)
{
    QElapsedTimer clock;
    clock.start();
    struct Acc
    {
        QElapsedTimer& c;
        ~Acc()
        {
            ++g_grabCount;
            g_grabMs += c.elapsed();
        }
    } acc{ clock };

    const QRect physical = physicalRect(screen, logicalRegion);

    QImage frame = blitRegion(physical, screen->name());
    if (frame.isNull()) {
        // Fall back to Qt: some display drivers refuse a BitBlt of a protected
        // surface, and Qt's path also handles the logical-pixel case.
        const QPixmap desktop = screen->grabWindow(0);
        if (desktop.isNull()) {
            return {};
        }
        const QRect clipped = physical.intersected(desktop.rect());
        if (clipped.isEmpty()) {
            return {};
        }
        frame = desktop.copy(clipped).toImage();
    }

    frame = frame.convertToFormat(QImage::Format_RGB32);
    // Everything downstream works in raw pixels. Without resetting the ratio,
    // QPainter::drawImage would draw the frame at its device-independent size
    // (1/1.5 of the pixels) and leave black bands around it.
    frame.setDevicePixelRatio(1.0);
    return frame;
}

struct MonitorLookup
{
    QString deviceName;
    RECT rect{ 0, 0, 0, 0 };
    bool found = false;
};

BOOL CALLBACK monitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* out = reinterpret_cast<MonitorLookup*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }
    const QString name = QString::fromWCharArray(info.szDevice);
    if (name.compare(out->deviceName, Qt::CaseInsensitive) == 0) {
        out->rect = info.rcMonitor;
        out->found = true;
        return FALSE;
    }
    return TRUE;
}

/// Physical (virtual desktop) rectangle of one monitor, looked up by the same
/// device name QScreen reports. Qt only exposes logical geometry, and scaling
/// that by a single DPR is wrong as soon as two monitors use different scale
/// factors.
RECT monitorPhysicalRect(const QString& deviceName)
{
    MonitorLookup lookup;
    lookup.deviceName = deviceName;
    EnumDisplayMonitors(
      nullptr, nullptr, monitorEnumProc, reinterpret_cast<LPARAM>(&lookup));
    return lookup.rect;
}

// SetCursorPos / WindowFromPoint expect physical virtual-desktop coordinates;
// Qt gives us logical coordinates relative to the monitor. Scaling by the DPR
// alone is not enough: the monitor itself may not start at the desktop origin.
QPoint physicalPoint(QScreen* screen, const QPoint& logical)
{
    const RECT monitor = monitorPhysicalRect(screen->name());
    const qreal dpr = screen->devicePixelRatio();
    const QRect logicalScreen = screen->geometry();
    return { monitor.left + qRound((logical.x() - logicalScreen.left()) * dpr),
             monitor.top + qRound((logical.y() - logicalScreen.top()) * dpr) };
}

QPoint physicalCenter(QScreen* screen, const QRect& logicalRegion)
{
    return physicalPoint(screen, logicalRegion.center());
}

QRect logicalFromPhysical(QScreen* screen, const RECT& physical)
{
    const RECT monitor = monitorPhysicalRect(screen->name());
    const qreal dpr = screen->devicePixelRatio();
    const QRect logicalScreen = screen->geometry();
    const int left = logicalScreen.left() +
                     qRound((physical.left - monitor.left) / dpr);
    const int top =
      logicalScreen.top() + qRound((physical.top - monitor.top) / dpr);
    const int right =
      logicalScreen.left() + qRound((physical.right - monitor.left) / dpr);
    const int bottom =
      logicalScreen.top() + qRound((physical.bottom - monitor.top) / dpr);
    return QRect(QPoint(left, top), QPoint(right - 1, bottom - 1));
}

// ---------------------------------------------------------------------------
// Which window is the user pointing at?
//
// WindowFromPoint on the centre of the selection answers a different question
// than "what can scroll here": the centre may sit on the wallpaper (Program
// Manager), on a fixed side pane, or on the window frame, and the injected
// wheel then goes nowhere - which is exactly how a scrolling capture silently
// produces a single-frame copy of the selection. Sampling a grid and letting
// the windows vote makes the answer follow the *contents* of the selection.
// ---------------------------------------------------------------------------
struct ScrollTarget
{
    HWND hwnd = nullptr;
    QRect logical;
    QString title;
    int votes = 0;

    bool valid() const { return hwnd != nullptr && !logical.isEmpty(); }
};

bool isDesktopShell(HWND window)
{
    wchar_t name[64] = {};
    if (GetClassNameW(window, name, 63) == 0) {
        return false;
    }
    const QString className = QString::fromWCharArray(name);
    return className == QLatin1String("Progman") ||
           className == QLatin1String("WorkerW") ||
           className == QLatin1String("Shell_TrayWnd") ||
           className == QLatin1String("Shell_SecondaryTrayWnd") ||
           className == QLatin1String("TaskListThumbnailWnd") ||
           className == QLatin1String("tooltips_class32");
}

// Remote-desktop viewers render a video stream of another machine. They have
// no scrollable UI Automation container, wheel events are forwarded to the
// remote host, and the resulting frames carry video-compression noise that
// defeats pixel-level stitching. Detecting this upfront saves the user from a
// long, confusing capture that can only produce a static image.
bool isRemoteDesktopViewer(HWND window)
{
    wchar_t title[256] = {};
    GetWindowTextW(window, title, 255);
    const QString t = QString::fromWCharArray(title);
    wchar_t cls[64] = {};
    GetClassNameW(window, cls, 63);
    const QString c = QString::fromWCharArray(cls);
    // UU远程 / ToDesk / 向日葵 / RDP / TeamViewer
    if (t.contains(QStringLiteral("UU远程"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("ToDesk"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("向日葵"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("TeamViewer"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("Remote Desktop"), Qt::CaseInsensitive) ||
        t.contains(QStringLiteral("远程桌面"), Qt::CaseInsensitive) ||
        c == QLatin1String("TscShellContainerClass")) {
        return true;
    }
    // The remote-session window itself has a user-chosen title ("work-win11",
    // ...), so a title match is not enough. Check the process image instead:
    // the viewer executable name is stable even when the window title is not.
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != 0 && pid != GetCurrentProcessId()) {
        HANDLE process =
          OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process != nullptr) {
            wchar_t image[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, image, &size)) {
                const QString exe =
                  QString::fromWCharArray(image).toLower();
                CloseHandle(process);
                return exe.contains(QStringLiteral("uu远程")) ||
                       exe.contains(QStringLiteral("uuremote")) ||
                       exe.contains(QStringLiteral("gameviewer")) ||
                       exe.contains(QStringLiteral("todesk")) ||
                       exe.contains(QStringLiteral("sunlogin")) ||
                       exe.contains(QStringLiteral("teamviewer")) ||
                       exe.contains(QStringLiteral("mstsc"));
            }
            CloseHandle(process);
        }
    }
    return false;
}

ScrollTarget resolveScrollTarget(QScreen* screen, const QRect& region)
{
    const DWORD self = GetCurrentProcessId();
    QHash<quintptr, ScrollTarget> candidates;

    // 7x7 interior grid: the border of the selection is where the user's mouse
    // was released, which is often a window edge and would win a tie.
    const int samples = 7;
    for (int iy = 1; iy < samples; ++iy) {
        for (int ix = 1; ix < samples; ++ix) {
            const QPoint logical(
              region.left() + region.width() * ix / samples,
              region.top() + region.height() * iy / samples);
            const QPoint physical = physicalPoint(screen, logical);
            POINT nativePoint{ physical.x(), physical.y() };
            HWND hit = WindowFromPoint(nativePoint);
            if (hit == nullptr) {
                continue;
            }
            HWND root = GetAncestor(hit, GA_ROOT);
            if (root == nullptr) {
                root = hit;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(root, &pid);
            // Our own HUD / overlay must never become the scroll target.
            if (pid == self || isDesktopShell(root)) {
                continue;
            }
            RECT frame{};
            if (!GetWindowRect(root, &frame)) {
                continue;
            }
            const QRect logicalFrame = logicalFromPhysical(screen, frame);
            if (!logicalFrame.intersects(region)) {
                continue;
            }
            ScrollTarget& entry = candidates[reinterpret_cast<quintptr>(root)];
            entry.hwnd = root;
            entry.logical = logicalFrame;
            entry.votes += 1;
            if (entry.title.isEmpty()) {
                wchar_t title[256] = {};
                GetWindowTextW(root, title, 255);
                entry.title = QString::fromWCharArray(title);
            }
        }
    }

    ScrollTarget best;
    qint64 bestCover = 0;
    for (const ScrollTarget& candidate : candidates) {
        // What matters is how much of the user's selection this window actually
        // covers: with a selection that spills over the wallpaper, a window
        // clipped to a 180px sliver can still collect the most grid hits and
        // would then be captured instead of the window the user framed
        // (2026-11-15). Intersection area first, votes as the tie-breaker.
        const qint64 cover =
          static_cast<qint64>(candidate.logical.intersected(region).width()) *
          candidate.logical.intersected(region).height();
        if (cover > bestCover ||
            (cover == bestCover && candidate.votes > best.votes) ||
            (cover == bestCover && candidate.votes == best.votes &&
             candidate.votes > 0 &&
             candidate.logical.width() * candidate.logical.height() <
               best.logical.width() * best.logical.height())) {
            best = candidate;
            bestCover = cover;
        }
    }
    return best;
}

// The centre of a selection is often a sidebar, a toolbar or an empty margin,
// so one probe point is not enough to answer "what can scroll here". Try the
// centre first, then the quadrants and the mid edges, and stop at the first
// point that lands inside a real scrollable container.
bool attachScrollContainer(ScrollUia& uia,
                           QScreen* screen,
                           const QRect& area,
                           QPoint* hitLogical)
{
    if (area.width() < 8 || area.height() < 8) {
        return false;
    }
    const int insetX = qMax(2, area.width() / 8);
    const int insetY = qMax(2, area.height() / 8);
    const QRect inner = area.adjusted(insetX, insetY, -insetX, -insetY);
    const QPoint middle = inner.center();
    const QVector<QPoint> candidates{
        area.center(),
        middle,
        QPoint(middle.x(), inner.top() + inner.height() / 4),
        QPoint(middle.x(), inner.bottom() - inner.height() / 4),
        QPoint(inner.left() + inner.width() / 4, middle.y()),
        QPoint(inner.right() - inner.width() / 4, middle.y()),
        QPoint(inner.left() + inner.width() / 5, inner.top() + inner.height() / 5),
        QPoint(inner.right() - inner.width() / 5,
               inner.top() + inner.height() / 5),
        QPoint(inner.left() + inner.width() / 5,
               inner.bottom() - inner.height() / 5),
        QPoint(inner.right() - inner.width() / 5,
               inner.bottom() - inner.height() / 5),
    };
    for (const QPoint& logical : candidates) {
        if (!area.contains(logical)) {
            continue;
        }
        const QPoint physical = physicalPoint(screen, logical);
        if (uia.attach(physical)) {
            if (hitLogical != nullptr) {
                *hitLogical = logical;
            }
            return true;
        }
    }
    return false;
}

QRect logicalFromPhysicalRect(QScreen* screen, const QRect& physical)
{
    RECT frame{ physical.left(),
                physical.top(),
                physical.right() + 1,
                physical.bottom() + 1 };
    return logicalFromPhysical(screen, frame);
}

// Dense per-pixel fingerprint used only to decide whether two grabs are the
// same picture. A sparse grid misses a 1px shift inside a flat band, which is
// exactly what "the smooth scroll has finished" looks like just before it
// really has.
quint64 denseSignature(const QImage& image)
{
    quint64 hash = 1469598103934665603ull;
    for (int y = 0; y < image.height(); y += 2) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += 3) {
            hash = (hash ^ static_cast<quint64>(qGray(row[x]))) *
                   1099511628211ull;
        }
    }
    return hash;
}

void movePointerTo(const QPoint& physical)
{
    const int screenW = qMax(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int screenH = qMax(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = qRound((physical.x() - originX) * 65535.0 /
                         qMax(1, screenW - 1));
    input.mi.dy = qRound((physical.y() - originY) * 65535.0 /
                         qMax(1, screenH - 1));
    SendInput(1, &input, sizeof(INPUT));
    pause(30);
}

// Bringing another process' window to the foreground can be refused by the
// foreground lock; attaching to its input queue lifts that restriction.
bool forceForeground(HWND window)
{
    if (window == nullptr) {
        return false;
    }
    if (GetForegroundWindow() == window) {
        return true;
    }
    SetForegroundWindow(window);
    if (GetForegroundWindow() == window) {
        return true;
    }

    const HWND current = GetForegroundWindow();
    const DWORD targetThread = GetWindowThreadProcessId(window, nullptr);
    const DWORD currentThread =
      current != nullptr ? GetWindowThreadProcessId(current, nullptr) : 0;
    const DWORD ownThread = GetCurrentThreadId();

    if (currentThread != 0 && currentThread != ownThread) {
        AttachThreadInput(currentThread, ownThread, TRUE);
    }
    if (targetThread != ownThread) {
        AttachThreadInput(targetThread, ownThread, TRUE);
    }

    BringWindowToTop(window);
    SetForegroundWindow(window);
    SetActiveWindow(window);

    if (targetThread != ownThread) {
        AttachThreadInput(targetThread, ownThread, FALSE);
    }
    if (currentThread != 0 && currentThread != ownThread) {
        AttachThreadInput(currentThread, ownThread, FALSE);
    }
    return GetForegroundWindow() == window;
}

void sendKey(WORD vk, bool control = false)
{
    INPUT inputs[4]{};
    int count = 0;
    if (control) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_CONTROL;
        ++count;
    }
    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = vk;
    ++count;
    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = vk;
    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
    ++count;
    if (control) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_CONTROL;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }
    SendInput(count, inputs, sizeof(INPUT));
}

void scrollDown(const QPoint& centerPhysical,
                HWND target,
                int mode,
                int notches)
{
    movePointerTo(centerPhysical);

    if (mode == 2) {
        // Wheel events are filtered out on some virtual display drivers while
        // keyboard scrolling still works, so PageDown is the fallback.
        forceForeground(target);
        sendKey(VK_NEXT);
        return;
    }

    if (mode == 1 && target != nullptr) {
        // Some applications only react to a wheel message sent directly to
        // their window rather than to injected input.
        PostMessageW(target,
                     WM_MOUSEWHEEL,
                     MAKEWPARAM(0,
                                static_cast<short>(-WHEEL_DELTA * notches)),
                     MAKELPARAM(centerPhysical.x(), centerPhysical.y()));
        return;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA * notches);
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("SendInput wheel failed, GetLastError=%1")
               .arg(GetLastError());
    }
}

// Smooth scrolling keeps repainting for a few hundred milliseconds after the
// wheel event. Grabbing mid animation produces a half scrolled frame, which
// either tears the stitch or reports a bogus shift. Wait until the frame has
// been identical for a continuous span before using it.
//
// The settle window (150ms minimum + 240ms stable span) is sized for pages
// that animate. Pages that repaint instantly - terminals, native lists, most
// desktop apps - still pay that tax on every frame, which is most of the
// 0.9s/frame cost. The tuner below watches how each frame settled: after a
// run of frames that were already quiet at the first re-grab it shrinks the
// windows, and the first frame that repaints late restores the conservative
// values for the rest of the capture.
struct SettleTuner
{
    int quietStreak = 0;
    int minSettle = MIN_SETTLE_MS;
    int stableSpan = STABLE_SPAN_MS;
};
QImage stableFrame(QScreen* screen,
                   const QRect& region,
                   int timeoutMs,
                   SettleTuner* tuner = nullptr)
{
    const int minSettle = tuner ? tuner->minSettle : MIN_SETTLE_MS;
    const int stableSpan = tuner ? tuner->stableSpan : STABLE_SPAN_MS;
    QImage last = grabFrame(screen, region);
    if (last.isNull()) {
        return last;
    }
    quint64 lastSum = denseSignature(last);
    int stableSince = minSettle;
    bool changedAfterMinSettle = false;

    if (minSettle > 0) {
        pause(minSettle);
        const QImage again = grabFrame(screen, region);
        if (again.isNull() || again.size() != last.size()) {
            return last;
        }
        const quint64 sum = denseSignature(again);
        if (sum != lastSum) {
            stableSince = minSettle;
            changedAfterMinSettle = true;
        }
        last = again;
        lastSum = sum;
    }

    for (int waited = minSettle; waited < timeoutMs;
         waited += SETTLE_POLL_MS) {
        pause(SETTLE_POLL_MS);
        const QImage next = grabFrame(screen, region);
        if (next.isNull() || next.size() != last.size()) {
            return last;
        }
        const quint64 sum = denseSignature(next);
        if (sum != lastSum) {
            stableSince = waited + SETTLE_POLL_MS;
            changedAfterMinSettle = true;
            lastSum = sum;
        }
        last = next;
        if (waited + SETTLE_POLL_MS - stableSince >= stableSpan) {
            if (tuner != nullptr) {
                if (changedAfterMinSettle) {
                    if (tuner->quietStreak > 0 || tuner->minSettle != MIN_SETTLE_MS) {
                        AbstractLogger::info(AbstractLogger::LogFile)
                          << QStringLiteral(
                               "Settle tuner: page repaints late, restoring "
                               "conservative windows");
                    }
                    tuner->quietStreak = 0;
                    tuner->minSettle = MIN_SETTLE_MS;
                    tuner->stableSpan = STABLE_SPAN_MS;
                } else if (++tuner->quietStreak >= 4 &&
                           tuner->minSettle > 90) {
                    tuner->minSettle = 90;
                    tuner->stableSpan = 150;
                    AbstractLogger::info(AbstractLogger::LogFile)
                      << QStringLiteral(
                           "Settle tuner: %1 quiet frames in a row, settle "
                           "windows tightened to 90+150ms")
                           .arg(tuner->quietStreak);
                }
            }
            return last;
        }
    }
    return last;
}

// A just revealed window can still present a blank composited frame; that
// would be stitched in as a black gap. Retry until the region has content.
bool looksBlank(const QImage& image)
{
    if (image.isNull()) {
        return true;
    }
    const int stepY = qMax(1, image.height() / 24);
    const int stepX = qMax(1, image.width() / 24);
    qint64 total = 0;
    int count = 0;
    for (int y = 0; y < image.height(); y += stepY) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += stepX) {
            total += qGray(row[x]);
            ++count;
        }
    }
    return count == 0 || (total / count) < 12;
}

bool escapePressed()
{
    // 0x8000 is "currently down", 0x0001 is "pressed since the previous call".
    // Without the latch a quick tap between two polls is missed entirely.
    const SHORT state = GetAsyncKeyState(VK_ESCAPE);
    return (state & 0x8000) != 0 || (state & 0x0001) != 0;
}

// ---------------------------------------------------------------------------
// Row profile: one greyscale signature per (row, column sample). H x 48 bytes
// for a 1100px tall frame, so comparing frames costs a few hundred thousand
// integer operations instead of several million pixel reads.
// ---------------------------------------------------------------------------
struct RowProfile
{
    int rows = 0;
    int cols = 0;
    QVector<quint8> data;

    quint8 at(int y, int c) const { return data[y * cols + c]; }
    bool isEmpty() const { return rows <= 0 || cols <= 0; }
};

RowProfile buildProfile(const QImage& image)
{
    RowProfile profile;
    if (image.isNull()) {
        return profile;
    }
    const int h = image.height();
    const int w = image.width();
    const int left = w * LEFT_MARGIN_PERCENT / 100;
    const int right = w - w * RIGHT_MARGIN_PERCENT / 100;
    const int span = right - left;
    if (span < 16 || h < 16) {
        return profile;
    }
    const int cols =
      qBound(8, qMin(PROFILE_COLUMNS, span / 2), PROFILE_COLUMNS);
    QVector<int> xs(cols);
    for (int c = 0; c < cols; ++c) {
        xs[c] = left + int(qint64(c) * (span - 1) / qMax(1, cols - 1));
    }

    profile.rows = h;
    profile.cols = cols;
    profile.data.resize(h * cols);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        quint8* out = profile.data.data() + y * cols;
        for (int c = 0; c < cols; ++c) {
            const int x = xs[c];
            const int xm = qMax(0, x - 1);
            const int xp = qMin(w - 1, x + 1);
            out[c] =
              quint8((qGray(row[x]) * 2 + qGray(row[xm]) + qGray(row[xp])) / 4);
        }
    }
    return profile;
}

struct ColumnStats
{
    QVector<int> activity;
    QVector<int> variance;
};

// activity: how much a column changed between the two frames at unchanged row
// positions. Static sidebars score ~0. variance: how varied the column is;
// a flat column matches at every offset and carries no information.
ColumnStats analyseColumns(const RowProfile& previous, const RowProfile& current)
{
    const int cols = qMin(previous.cols, current.cols);
    const int rows = qMin(previous.rows, current.rows);
    ColumnStats stats;
    stats.activity.resize(cols);
    stats.variance.resize(cols);
    for (int c = 0; c < cols; ++c) {
        qint64 diff = 0;
        qint64 sum = 0;
        qint64 sumSq = 0;
        for (int y = 0; y < rows; ++y) {
            const int a = previous.at(y, c);
            const int b = current.at(y, c);
            diff += qAbs(a - b);
            sum += b;
            sumSq += qint64(b) * b;
        }
        const qint64 n = qMax<qint64>(1, rows);
        const double mean = double(sum) / double(n);
        const double variance = double(sumSq) / double(n) - mean * mean;
        stats.activity[c] = int(diff / n);
        stats.variance[c] = int(qMax(0.0, variance));
    }
    return stats;
}

QVector<int> usableColumns(const ColumnStats& stats)
{
    QVector<int> usable;
    for (int c = 0; c < stats.activity.size(); ++c) {
        if (stats.activity[c] >= COLUMN_ACTIVITY_MIN &&
            stats.variance[c] >= COLUMN_FLAT_VARIANCE) {
            usable << c;
        }
    }
    if (usable.size() >= MIN_USABLE_COLUMNS) {
        return usable;
    }
    // Pathological page (nearly uniform content): keep the most informative
    // columns rather than giving up on stitching.
    QVector<int> order(stats.activity.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&stats](int l, int r) {
        const int ls = stats.activity[l] * 8 + qMin(stats.variance[l], 400);
        const int rs = stats.activity[r] * 8 + qMin(stats.variance[r], 400);
        return ls > rs;
    });
    return order.mid(0, qMin(FALLBACK_COLUMNS, order.size()));
}

double rowDelta(const RowProfile& previous,
                const RowProfile& current,
                int y,
                const QVector<int>& columns)
{
    qint64 sum = 0;
    for (int c : columns) {
        sum += qAbs(int(previous.at(y, c)) - int(current.at(y, c)));
    }
    return double(sum) / qMax(1, columns.size());
}

struct Bars
{
    int top = 0;
    int bottom = 0;
};

// Runs of rows that are identical in both frames at the same position cannot
// be scrolling content: they sit above (header) or below (footer) the moving
// area. Excluding them is what stops a sticky bottom bar from pinning the
// measured shift to 0 for the rest of the capture.
Bars detectFixedBars(const RowProfile& previous,
                     const RowProfile& current,
                     const QVector<int>& columns,
                     int height)
{
    Bars bars;
    const int limit = height * FIXED_BAR_MAX_PERCENT / 100;
    if (limit <= FIXED_BAR_MIN_ROWS || columns.isEmpty()) {
        return bars;
    }
    // Diagnostic switch: reproduce the behaviour without fixed-bar exclusion.
    // A page with a sticky bottom bar then reports "no movement" on every
    // step and the capture is silently truncated.
    if (envFlag("FLAMESHOT_SCROLL_IGNORE_FIXED_BARS", false)) {
        return bars;
    }

    int top = 0;
    while (top < limit && rowDelta(previous, current, top, columns) <
                           FIXED_ROW_DELTA) {
        ++top;
    }
    if (top >= FIXED_BAR_MIN_ROWS && top < limit) {
        bars.top = top;
    }

    int bottom = 0;
    while (bottom < limit &&
           rowDelta(previous, current, height - 1 - bottom, columns) <
             FIXED_ROW_DELTA) {
        ++bottom;
    }
    if (bottom >= FIXED_BAR_MIN_ROWS && bottom < limit) {
        bars.bottom = bottom;
    }
    return bars;
}

struct Region
{
    int top = 0;
    int bottom = 0; // exclusive
    int height() const { return bottom - top; }
};

// Mean absolute difference of two frames at the same row positions, i.e. "did
// anything move at all". This is the only correct test for the end of the
// document; a failed match must not be confused with it.
double sameRowMad(const RowProfile& previous,
                  const RowProfile& current,
                  const QVector<int>& columns,
                  const Region& region)
{
    if (columns.isEmpty() || region.height() <= 0) {
        return std::numeric_limits<double>::max();
    }
    const int stepY = qMax(1, region.height() / SCORE_SAMPLED_ROWS);
    qint64 sum = 0;
    qint64 count = 0;
    for (int y = region.top; y < region.bottom; y += stepY) {
        for (int c : columns) {
            sum += qAbs(int(previous.at(y, c)) - int(current.at(y, c)));
            ++count;
        }
    }
    return count ? double(sum) / double(count)
                 : std::numeric_limits<double>::max();
}

// Score the hypothesis "content moved up by shift pixels". The whole available
// overlap is sampled, not a short band: a 240px window inside a repeating list
// matches at several offsets, and that is how a 300px step used to be measured
// as 287px.
double scoreShift(const RowProfile& previous,
                  const RowProfile& current,
                  int shift,
                  const QVector<int>& columns,
                  const Region& region)
{
    const int first = region.top + shift;
    if (shift < 0 || first >= region.bottom || columns.isEmpty()) {
        return std::numeric_limits<double>::max();
    }
    const int rows = region.bottom - first;
    const int stepY = qMax(1, rows / SCORE_SAMPLED_ROWS);
    qint64 sum = 0;
    qint64 count = 0;
    for (int y = first; y < region.bottom; y += stepY) {
        for (int c : columns) {
            sum += qAbs(int(previous.at(y, c)) -
                        int(current.at(y - shift, c)));
            ++count;
        }
    }
    return count ? double(sum) / double(count)
                 : std::numeric_limits<double>::max();
}

struct ShiftMatch
{
    int shift = 0;
    double mad = std::numeric_limits<double>::max();
    double rival = std::numeric_limits<double>::max();
    bool ambiguous = false;

    bool confident() const { return mad <= MAD_GOOD && !ambiguous; }
    bool perfect() const { return mad <= MAD_PERFECT && !ambiguous; }
};

// Wheel scrolling is nearly constant, so start at the previous step and expand
// outwards; accept as soon as the frames line up. CaptureOCR and wayscrollshot
// both do this, and it turns a full scan into a handful of candidates.
ShiftMatch findShift(const RowProfile& previous,
                     const RowProfile& current,
                     const QVector<int>& columns,
                     const Region& region,
                     int predicted)
{
    ShiftMatch best;
    if (columns.isEmpty() || region.height() < 32) {
        return best;
    }
    // A frame must keep at least a quarter of its content as overlap, otherwise
    // two frames no longer constrain each other.
    const int maxShift = qMax(0, region.height() - qMax(32, region.height() / 4));
    if (maxShift < MIN_SHIFT) {
        return best;
    }
    predicted = qBound(0, predicted, maxShift);

    QVector<int> order;
    order.reserve(maxShift + 1);
    order << predicted;
    for (int d = 1; d <= maxShift && order.size() < maxShift + 1; ++d) {
        if (predicted - d >= 0) {
            order << predicted - d;
        }
        if (predicted + d <= maxShift && order.size() < maxShift + 1) {
            order << predicted + d;
        }
    }

    double rivalMad = std::numeric_limits<double>::max();
    bool perfect = false;
    for (int candidate : order) {
        const double mad =
          scoreShift(previous, current, candidate, columns, region);
        const int previousBestShift = best.shift;
        if (mad < best.mad) {
            // The previous best becomes a rival if it is a distinct offset:
            // two near-equal minima mean the match is not trustworthy.
            if (best.mad < std::numeric_limits<double>::max() &&
                qAbs(candidate - previousBestShift) >= AMBIGUITY_MIN_GAP) {
                rivalMad = qMin(rivalMad, best.mad);
            }
            best.mad = mad;
            best.shift = candidate;
        } else if (qAbs(candidate - best.shift) >= AMBIGUITY_MIN_GAP) {
            rivalMad = qMin(rivalMad, mad);
        }
        if (best.mad <= MAD_PERFECT) {
            perfect = true;
        }
        // Only an exact hit justifies stopping the scan. Stopping at "good
        // enough" locks onto a neighbour one pixel away, which silently
        // accumulates as a one pixel gap per seam.
        if (best.mad == 0.0) {
            break;
        }
    }

    best.rival = rivalMad;
    best.ambiguous = !perfect && rivalMad < std::numeric_limits<double>::max() &&
                     best.mad > rivalMad * AMBIGUITY_RATIO;
    return best;
}

// Growable RGB32 canvas. Appending into a pre-allocated buffer keeps the
// stitcher linear; rebuilding a QImage per strip is quadratic and huge pages
// spent most of their time copying.
struct Canvas
{
    QImage image;
    int width = 0;
    int used = 0;

    void reset(int w)
    {
        image = QImage();
        width = w;
        used = 0;
    }

    bool reserve(int rows, int maxHeight)
    {
        if (width <= 0) {
            return false;
        }
        if (!image.isNull() && rows <= image.height()) {
            return true;
        }
        const int current = image.isNull() ? 0 : image.height();
        int want = qMax(rows, qMax(2048, current * 2));
        want = qMin(want, maxHeight);
        if (want < rows) {
            return false;
        }
        QImage bigger(width, want, QImage::Format_RGB32);
        if (bigger.isNull()) {
            return false;
        }
        bigger.setDevicePixelRatio(1.0);
        const int bytes = width * 4;
        for (int y = 0; y < used && y < current; ++y) {
            std::memcpy(bigger.scanLine(y), image.constScanLine(y), bytes);
        }
        image = bigger;
        return true;
    }

    bool append(const QImage& source, int sourceY, int rows, int maxHeight)
    {
        if (rows <= 0) {
            return true;
        }
        if (source.width() != width || sourceY < 0 ||
            sourceY + rows > source.height()) {
            return false;
        }
        if (!reserve(used + rows, maxHeight)) {
            return false;
        }
        const int bytes = width * 4;
        for (int y = 0; y < rows; ++y) {
            std::memcpy(image.scanLine(used + y),
                        source.constScanLine(sourceY + y),
                        bytes);
        }
        used += rows;
        return true;
    }

    void rewind(int rows) { used = qMax(0, used - rows); }

    QImage result() const
    {
        if (image.isNull() || used <= 0) {
            return {};
        }
        return image.copy(0, 0, width, used);
    }
};

// --- fixed side panels -----------------------------------------------------
// A chat sidebar, a navigation tree or a tool palette lives inside the window
// and never scrolls. Every stitched strip therefore pastes it again, and the
// long image ends up showing the same sidebar a dozen times (2026-11-15: a
// Codex conversation came back with its conversation list repeated 12x).
// UIA does not always expose the inner scrollable pane, and the window client
// rect always contains the sidebar, so the dependable signal is the pixels:
// columns that stay byte-identical while the content columns change.
struct SideBands
{
    int left = 0;
    int right = 0;
    int width() const { return left + right; }
};

SideBands detectStaticSideBands(const QImage& before, const QImage& after)
{
    SideBands bands;
    if (before.isNull() || after.isNull() || before.size() != after.size()) {
        return bands;
    }
    const int w = before.width();
    const int h = before.height();
    if (w < 96 || h < 96) {
        return bands;
    }
    const QImage a = before.convertToFormat(QImage::Format_RGB32);
    const QImage b = after.convertToFormat(QImage::Format_RGB32);

    const int stepY = qMax(1, h / 240);
    QVector<double> diff(w, 0.0);
    QVector<double> sum(w, 0.0);
    QVector<double> sumSq(w, 0.0);
    QVector<double> changed(w, 0.0);
    int sampled = 0;
    for (int y = 0; y < h; y += stepY) {
        const auto* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb ca = ra[x];
            const QRgb cb = rb[x];
            // Colour distance, not greyscale: two different palette colours
            // (blue #1f6feb and green #238636) share almost the same grey, so
            // a grey-only test reads scrolling content as "static".
            const int d = (qAbs(qRed(ca) - qRed(cb)) +
                           qAbs(qGreen(ca) - qGreen(cb)) +
                           qAbs(qBlue(ca) - qBlue(cb))) /
                          3;
            diff[x] += d;
            if (d > 24) {
                changed[x] += 1.0;
            }
            const int gb = qGray(cb);
            sum[x] += gb;
            sumSq[x] += double(gb) * gb;
        }
        ++sampled;
    }
    if (sampled <= 0) {
        return bands;
    }

    const double n = double(sampled);
    for (int x = 0; x < w; ++x) {
        changed[x] /= n;
    }
    // A column is "static" when it did not change between the two frames.
    // Content check happens per band, not per column: a sidebar contains
    // glyph-free columns (gaps between words) and requiring every single
    // column to carry ink would break the run at the first gap.
    auto isStatic = [&](int x) { return diff[x] / n <= 2.0; };
    auto varianceOf = [&](int from, int to) {
        double total = 0.0;
        for (int x = from; x < to; ++x) {
            const double mean = sum[x] / n;
            total += qMax(0.0, sumSq[x] / n - mean * mean);
        }
        return to > from ? total / double(to - from) : 0.0;
    };

    int left = 0;
    while (left < w && isStatic(left)) {
        ++left;
    }

    // Right side: a fixed panel, a page gutter and the vertical scrollbar share
    // one property - they do not scroll with the content, so each one would be
    // pasted once per frame (2026-11-15: the scrollbar thumb came back repeated
    // every 300px down the long image). Scanning for columns that barely
    // change catches all three; the guard makes the band stop at a column that
    // really does scroll, so a plain sparse page is left untouched.
    const int rightLimit = int(w * 0.25);
    int right = 0;
    while (right < rightLimit && changed[w - 1 - right] < 0.6) {
        ++right;
    }
    const int rightRaw = right;
    if (right > 0) {
        const int inner = w - right - 1;
        if (inner < 0 || changed[inner] < 0.75) {
            right = 0;
        }
    }
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Right edge probe: raw=%1 kept=%2 edge=%3 inline=%4 "
                        "w-40=%5")
           .arg(rightRaw)
           .arg(right)
           .arg(changed[w - 1], 0, 'f', 2)
           .arg(rightRaw > 0 ? changed[w - rightRaw - 1] : -1.0, 0, 'f', 2)
           .arg(w > 40 ? changed[w - 40] : -1.0, 0, 'f', 2);
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Static column runs: left=%1px right=%2px of %3px")
           .arg(left)
           .arg(right)
           .arg(w);

    const int minBand = qMax(8, int(w * 0.05));
    if (left < minBand || left > int(w * 0.45)) {
        left = 0;
    }
    if (right < qMax(6, int(w * 0.02)) || right > rightLimit) {
        right = 0;
    }
    // A flat margin is static too (white gutter, page background). Cropping it
    // would only make the result narrower without removing anything, so a band
    // has to contain actual ink.
    if (left > 0 && varianceOf(0, left) < 8.0) {
        left = 0;
    }
    // Never eat the content column: whatever remains has to stay usable.
    if (w - left - right < qMax(120, int(w * 0.35))) {
        left = 0;
        right = 0;
    }
    bands.left = left;
    bands.right = right;
    return bands;
}

QString exportResult(const QImage& image)
{
    QString directory = ConfigHandler().savePath();
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    QDir().mkpath(directory);

    const QString fileName =
      QStringLiteral("flameshot-scroll-%1.png")
        .arg(QDateTime::currentDateTime().toString(
          QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QDir(directory).filePath(fileName);
    if (!image.save(path, "PNG")) {
        return {};
    }

    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard) {
        clipboard->setPixmap(QPixmap::fromImage(image));
    }
    return path;
}

} // namespace

ScrollCapture* ScrollCapture::instance()
{
    static ScrollCapture capture;
    return &capture;
}

ScrollCapture::ScrollCapture(QObject* parent)
  : QObject(parent)
{}

void ScrollCapture::start()
{
    start(QRect());
}

void ScrollCapture::start(const QRect& preselectedRegion)
{
    if (m_running) {
        return;
    }
    m_running = true;
    struct Guard
    {
        bool& flag;
        ~Guard() { flag = false; }
    } guard{ m_running };

    QRect region = preselectedRegion;
    QScreen* screen = nullptr;
    if (!region.isEmpty()) {
        // Called from the editor, which already asked the user for a region.
        // Resolve the monitor from the region itself: the mouse may have been
        // parked anywhere by the time the overlay closed.
        screen = QGuiApplication::screenAt(region.center());
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scrolling capture: reusing the editor selection");
        if (screen == nullptr) {
            region = QRect();
        }
    }

    if (region.isEmpty()) {
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scrolling capture: waiting for region selection");

        RegionSelector selector;
        if (selector.exec() != QDialog::Accepted) {
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Scrolling capture: selection cancelled");
            emit finished(false, QStringLiteral("selection cancelled"));
            return;
        }
        region = selector.selected();
        // exec() already hides the dialog, but make it explicit: the overlay
        // must not appear in the first frame nor swallow the wheel events.
        selector.hide();
        QCoreApplication::processEvents();
        screen = selector.screen();
    }

    if (screen == nullptr) {
        emit finished(false, QStringLiteral("no screen under the cursor"));
        return;
    }

    // A selection can stick out of the monitor by a few pixels (the editor
    // clamps the rubber band to its own window, which is not always the screen
    // rectangle). Blitting outside the screen yields black rows, so clamp.
    const QRect screenGeometry = screen->geometry();
    if (!screenGeometry.contains(region)) {
        const QRect clamped = region.intersected(screenGeometry);
        if (clamped.width() < MIN_REGION_SIZE ||
            clamped.height() < MIN_REGION_SIZE) {
            emit finished(false, QStringLiteral("selection is outside the screen"));
            return;
        }
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Selection clamped to the screen: (%1,%2 %3x%4)")
               .arg(clamped.x())
               .arg(clamped.y())
               .arg(clamped.width())
               .arg(clamped.height());
        region = clamped;
    }

    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Scroll selection logical=(%1,%2 %3x%4) dpr=%5 screen=(%6,%7 %8x%9)")
             .arg(region.x())
             .arg(region.y())
             .arg(region.width())
             .arg(region.height())
             .arg(screen->devicePixelRatio())
             .arg(screen->geometry().x())
             .arg(screen->geometry().y())
             .arg(screen->geometry().width())
             .arg(screen->geometry().height());

    // The editor hands its selection over and closes itself. Sampling while
    // our own fullscreen overlay is still on screen would answer "flameshot",
    // which is exactly the window we must not scroll. Wait until the point is
    // back over somebody else's pixels.
    {
        const QPoint probe = physicalCenter(screen, region);
        const DWORD self = GetCurrentProcessId();
        for (int waited = 0; waited < 1500; waited += 50) {
            POINT nativePoint{ probe.x(), probe.y() };
            HWND under = WindowFromPoint(nativePoint);
            DWORD pid = 0;
            if (under != nullptr) {
                GetWindowThreadProcessId(under, &pid);
            }
            if (pid != self) {
                break;
            }
            pause(50);
        }
    }

    // --- 1. which window is the user pointing at? --------------------------
    // The editor closed a few hundred milliseconds ago, so sample now: the
    // answer has to be a real application window, not our own leftover
    // overlay and not the wallpaper.
    const ScrollTarget target = resolveScrollTarget(screen, region);
    if (!target.valid()) {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("Scrolling capture: no application window inside "
                            "the selection, nothing can scroll here");
        CaptureHud hud(region, screen);
        hud.setTitle(ZAG_TR("No scrollable content"));
        hud.setStatus(
          ZAG_TR("No window inside the selection. Frame a page or document content area."));
        pause(1900);
        emit finished(false, QStringLiteral("no window inside the selection"));
        return;
    }
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Scroll target window: %1 (votes=%2, frame=(%3,%4 "
                        "%5x%6))")
           .arg(target.title)
           .arg(target.votes)
           .arg(target.logical.x())
           .arg(target.logical.y())
           .arg(target.logical.width())
           .arg(target.logical.height());

    // --- 1b. clip the capture rectangle to the target window's client area -
    // The rubber band is only a hint. Whatever lies outside the target window
    // (desktop wallpaper, taskbar, a neighbouring app) never scrolls, so those
    // rows would be copied down the whole stitched image and the static pixels
    // would be read as "reached bottom" on the first step. Clip BEFORE a single
    // pixel is grabbed: 2026-11-15 a selection that spilled over the wallpaper
    // produced a long image with the entire desktop stacked behind the window.
    auto clientRectOf = [&](HWND window) {
        RECT client{};
        if (window == nullptr || !GetClientRect(window, &client)) {
            return target.logical;
        }
        POINT topLeft{ client.left, client.top };
        POINT bottomRight{ client.right, client.bottom };
        ClientToScreen(window, &topLeft);
        ClientToScreen(window, &bottomRight);
        const RECT physical{ topLeft.x, topLeft.y, bottomRight.x,
                             bottomRight.y };
        return logicalFromPhysical(screen, physical);
    };
    const QRect windowClient = clientRectOf(target.hwnd);
    {
        const QRect clipped = region.intersected(windowClient);
        if (clipped.width() >= MIN_REGION_SIZE &&
            clipped.height() >= MIN_REGION_SIZE) {
            if (clipped != region) {
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Selection clipped to the window client "
                                    "area: (%1,%2 %3x%4) -> (%5,%6 %7x%8)")
                       .arg(region.x())
                       .arg(region.y())
                       .arg(region.width())
                       .arg(region.height())
                       .arg(clipped.x())
                       .arg(clipped.y())
                       .arg(clipped.width())
                       .arg(clipped.height());
                region = clipped;
            }
        } else if (!windowClient.intersects(region)) {
            CaptureHud bad(region, screen);
            bad.setBadge(QColor(239, 68, 68));
            bad.setTitle(ZAG_TR("Selection is outside the window"));
            bad.setStatus(ZAG_TR("Frame the whole content window and try again"));
            pause(2200);
            emit finished(false,
                          QStringLiteral("selection is outside the target "
                                         "window"));
            return;
        }
    }

    // Remote-desktop viewers show a video stream, not a real scrollable
    // document. Warn early so the user knows this is a fundamental limitation,
    // not a bug.
    if (isRemoteDesktopViewer(target.hwnd)) {
        AbstractLogger::warning(AbstractLogger::LogFile)
          << QStringLiteral("Target is a remote-desktop viewer (%1): scrolling "
                            "capture is not supported on video streams")
               .arg(target.title);
        CaptureHud warnHud(region, screen);
        warnHud.setBadge(QColor(239, 68, 68));
        warnHud.setTitle(ZAG_TR("Remote desktop windows are not supported"));
        warnHud.setStatus(
          ZAG_TR("Install Flameshot on the remote machine instead"));
        pause(3000);
        emit finished(false,
                      QStringLiteral("remote desktop viewer: %1")
                        .arg(target.title));
        return;
    }

    // Declared before the rewind phase so its progress text is visible while
    // the page is wheeled back to the top (a long page can take half a
    // minute, and total silence there reads as "the app froze").
    CaptureHud hud(region, screen);
    hud.setStatus(ZAG_TR("Detecting scroll container…"));
    QCoreApplication::processEvents();

    // A window that sits behind another one would be blitted partially hidden,
    // and Chromium also stops painting occluded surfaces. Bring it forward,
    // but only when needed so a focused window is not disturbed.
    const HWND foreground = GetForegroundWindow();
    const HWND foregroundRoot =
      foreground != nullptr ? GetAncestor(foreground, GA_ROOT) : nullptr;
    if (foregroundRoot != target.hwnd) {
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Target window is not in the foreground, raising it");
        forceForeground(target.hwnd);
        pause(240);
        // The target window just jumped above every overlay. Re-raise the HUD
        // so the user still sees "识别滚动容器中…" instead of a silent void
        // (2026-11-15: "滚着滚着交互界面消失了" was exactly this).
        hud.raise();
        QCoreApplication::processEvents();
    }

    // The wheel has to land on scrollable content. Clamp the selection to the
    // window, drop the frame, and keep clear of the scrollbar: over the
    // scrollbar several applications scroll by a different amount (or not at
    // all).
    auto contentFor = [&](const QRect& rect) {
        QRect c = rect.intersected(target.logical);
        if (c.width() < MIN_REGION_SIZE || c.height() < MIN_REGION_SIZE) {
            c = rect;
        }
        c.adjust(2, 2, -2, -2);
        const int guard = qMin(20, qMax(0, c.width() / 3));
        c.adjust(0, 0, -guard, 0);
        if (c.width() < MIN_REGION_SIZE || c.height() < MIN_REGION_SIZE) {
            c = rect;
        }
        return c;
    };
    QRect content = contentFor(region);
    QPoint scrollPoint = physicalCenter(screen, content);
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Scroll content area=(%1,%2 %3x%4) point=(%5,%6)")
           .arg(content.x())
           .arg(content.y())
           .arg(content.width())
           .arg(content.height())
           .arg(scrollPoint.x())
           .arg(scrollPoint.y());

    // --- 2. find the scroll container -------------------------------------
    // UI Automation answers the actual question - "what can scroll here?" -
    // and can drive it without touching the pointer or stealing the keyboard
    // focus. When it is available the capture knows the exact position and
    // the visible fraction instead of guessing from pixel deltas.
    ScrollUia uia;
    bool uiaReady = false;
    double uiaPercent = -1.0;
    double uiaView = -1.0;
    double uiaStep = 0.0;
    QPoint wheelPoint = scrollPoint;
    QPoint uiaProbe;
    const bool uiaAttached =
      envFlag("FLAMESHOT_SCROLL_UIA", true) &&
      attachScrollContainer(uia, screen, content, &uiaProbe);
    if (uiaAttached) {
        uiaView = uia.viewPercent();
        uiaPercent = uia.verticalPercent();
        // The container knows its own rectangle. Scrolling inside it is more
        // reliable than scrolling inside the selection, which may include
        // chrome (tabs, sidebars) that swallows wheel events.
        const QRect container =
          logicalFromPhysicalRect(screen, uia.boundsPhysical());
        if (container.width() >= MIN_REGION_SIZE &&
            container.height() >= MIN_REGION_SIZE &&
            container.intersects(content)) {
            wheelPoint = physicalCenter(screen, container.intersected(content));
        }
        if (uiaView > 0.0 && uiaPercent >= 0.0) {
            uiaReady = true;
            // Never step by a full viewport: consecutive frames need an
            // overlap for the stitch to find the seam.
            uiaStep = qBound(8.0, uiaView * 0.45, 45.0);
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Scrolling with UI Automation: %1, step=%2%, "
                                "probe=(%3,%4) wheel-point=(%5,%6)")
                   .arg(uia.description())
                   .arg(uiaStep, 0, 'f', 1)
                   .arg(uiaProbe.x())
                   .arg(uiaProbe.y())
                   .arg(wheelPoint.x())
                   .arg(wheelPoint.y());
        }
    }
    if (!uiaReady) {
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scrolling with injected wheel events (no "
                            "scrollable UI Automation element under the "
                            "selection)");
    }

    // UI Automation is an excellent *detector* (container rect, wheel point,
    // scroll position, bottom-of-document) but a fragile *driver*:
    // SetScrollPercent positions by percent of the whole document, so on a
    // long page a single 27%-step jumps thousands of pixels and leaves zero
    // overlap for the stitcher (matcher window is ±45px). Industry tools
    // (ShareX and friends) scroll with real wheel input for exactly this
    // reason. UIA driving remains available as an opt-in escape hatch.
    bool uiaDrive =
      uiaReady && envFlag("FLAMESHOT_SCROLL_UIA_DRIVER", false);
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Scroll driver: %1")
           .arg(uiaDrive ? QStringLiteral("UI Automation (opt-in)")
                         : QStringLiteral("injected wheel events"));

    // --- 2b. let the user confirm, and correct, the scrolling area ---------
    // Detection is a suggestion, not a verdict. The overlay shows the container
    // that was found, lets the user move/resize the rectangle, and only starts
    // the capture after an explicit confirmation. Skipping it produces the
    // "it scrolled the wallpaper" class of failure with no way to intervene.
    if (envFlag("FLAMESHOT_SCROLL_CONFIRM", true)) {
        QRect suggestion = region;
        QString detectedLabel;
        if (uiaAttached) {
            const QRect container =
              logicalFromPhysicalRect(screen, uia.boundsPhysical());
            const QRect usable = container.intersected(screen->geometry());
            if (usable.width() >= MIN_REGION_SIZE &&
                usable.height() >= MIN_REGION_SIZE &&
                usable.intersects(region)) {
                suggestion = usable;
                detectedLabel = uia.description();
            }
        }
        // Never suggest a rectangle that reaches outside the window: the
        // capture clips to the client area anyway, and showing the user a
        // wider frame than what will be grabbed is exactly how "对象不对"
        // reports start.
        if (suggestion.intersected(windowClient).width() >= MIN_REGION_SIZE &&
            suggestion.intersected(windowClient).height() >= MIN_REGION_SIZE) {
            suggestion = suggestion.intersected(windowClient);
        }
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral(
               "Scroll region confirmation shown: initial=(%1,%2 %3x%4) "
               "suggestion=(%5,%6 %7x%8) detected=%9")
               .arg(region.x())
               .arg(region.y())
               .arg(region.width())
               .arg(region.height())
               .arg(suggestion.x())
               .arg(suggestion.y())
               .arg(suggestion.width())
               .arg(suggestion.height())
               .arg(detectedLabel.isEmpty() ? QStringLiteral("no")
                                            : detectedLabel);
        // Snap the starting rectangle onto the container UI Automation
        // found: the user asked for the scrollable panel, and aligning the
        // selection by hand is exactly the fiddling this feature should
        // remove. The rectangle stays fully draggable, so a wrong detection
        // costs one drag instead of the whole capture.
        const bool snappedToContainer =
          !detectedLabel.isEmpty() && suggestion != region;
        ScrollRegionPicker picker(screen,
                                  snappedToContainer ? suggestion : region,
                                  suggestion,
                                  detectedLabel);
        if (!picker.exec()) {
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Scroll region confirmation cancelled by the "
                                "user");
            emit finished(
              false, QStringLiteral("cancelled at the region confirmation"));
            return;
        }
        const QRect confirmed = picker.selected();
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scroll region confirmed: (%1,%2 %3x%4)")
               .arg(confirmed.x())
               .arg(confirmed.y())
               .arg(confirmed.width())
               .arg(confirmed.height());
        if (confirmed != region && confirmed.width() >= MIN_REGION_SIZE &&
            confirmed.height() >= MIN_REGION_SIZE) {
            region = confirmed;
            // The user can drag the rectangle back over the desktop; clip it
            // again so the frame is always the window content, never the
            // wallpaper behind it.
            const QRect insideWindow = region.intersected(windowClient);
            if (insideWindow.width() >= MIN_REGION_SIZE &&
                insideWindow.height() >= MIN_REGION_SIZE &&
                insideWindow != region) {
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Confirmed region clipped to the window: "
                                    "(%1,%2 %3x%4) -> (%5,%6 %7x%8)")
                       .arg(region.x())
                       .arg(region.y())
                       .arg(region.width())
                       .arg(region.height())
                       .arg(insideWindow.x())
                       .arg(insideWindow.y())
                       .arg(insideWindow.width())
                       .arg(insideWindow.height());
                region = insideWindow;
            }
            content = contentFor(region);
            scrollPoint = physicalCenter(screen, content);
            if (uiaAttached) {
                const QRect container =
                  logicalFromPhysicalRect(screen, uia.boundsPhysical());
                const QRect inside = container.intersected(content);
                if (inside.width() >= MIN_REGION_SIZE &&
                    inside.height() >= MIN_REGION_SIZE) {
                    wheelPoint = physicalCenter(screen, inside);
                } else {
                    wheelPoint = physicalCenter(screen, content);
                }
            }
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Scroll content after confirmation=(%1,%2 "
                                "%3x%4) wheel-point=(%5,%6)")
                   .arg(content.x())
                   .arg(content.y())
                   .arg(content.width())
                   .arg(content.height())
                   .arg(wheelPoint.x())
                   .arg(wheelPoint.y());
        }
    }

    // --- 3. start from the top of the document ----------------------------
    // Visible confirmation of what is about to be scrolled (ShareX keeps its
    // region form up for the whole run). The border is drawn outside the
    // captured rectangle and dies with this function, so it never lands in a
    // frame nor outlives the capture.
    std::unique_ptr<RegionOutline> outline(
      new RegionOutline(screen, region));
    hud.raise();
    QCoreApplication::processEvents();

    if (uiaDrive && envFlag("FLAMESHOT_SCROLL_FROM_TOP", true) &&
        uiaPercent > 0.6) {
        const double before = uiaPercent;
        uia.scrollToPercent(0.0);
        pause(240);
        const double rewound = uia.verticalPercent();
        if (rewound >= 0.0) {
            uiaPercent = rewound;
        }
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Rewinding to the top: %1% -> %2%")
               .arg(before, 0, 'f', 1)
               .arg(uiaPercent, 0, 'f', 1);
        if (before > 2.0 && rewound > before - 0.5) {
            // The provider advertises a scroll pattern but ignores absolute
            // positioning. Sub-pixel stitching needs a known origin, so drive
            // the wheel instead.
            AbstractLogger::warning(AbstractLogger::LogFile)
              << QStringLiteral("UI Automation refuses absolute scrolling "
                                "(stuck at %1%), using the wheel")
                   .arg(rewound, 0, 'f', 1);
            uiaReady = false;
            uiaDrive = false;
        }
    }

    QImage first;
    for (int attempt = 0; attempt < 10; ++attempt) {
        first = stableFrame(screen, region, SETTLE_TIMEOUT_MS);
        if (!first.isNull() && !looksBlank(first)) {
            break;
        }
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("First frame still blank, retrying (%1)")
               .arg(attempt + 1);
        pause(150);
    }
    if (first.isNull()) {
        emit finished(false, QStringLiteral("could not grab the first frame"));
        return;
    }
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("First frame %1x%2").arg(first.width()).arg(first.height());

    const int notches = notchesForRegion(region.height());
    // Adapted while running: a page zoomed in moves far more pixels per notch
    // than the region height suggests, and huge steps fall out of the row
    // matcher's comfort zone (2026-11-15: 2 notches scrolled 782px per frame
    // on a zoomed ChatGPT page, degrading 3 of 8 seams).
    int stepNotches = notches;
    int scrollMode = 0; // 0=injected wheel, 1=posted WM_MOUSEWHEEL, 2=PageDown
    if (!uiaDrive) {
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scroll step: %1 notches for a %2px tall region")
               .arg(notches)
               .arg(region.height());

        // A scrolling capture is meant to cover the whole document, so rewind
        // to the top when the target is already scrolled. Probe upwards
        // first: one notch that changes nothing proves we are already at the
        // top and the capture starts exactly where the user pointed it.
        if (envFlag("FLAMESHOT_SCROLL_FROM_TOP", true)) {
            // Park the pointer and let it settle BEFORE taking the reference
            // frame: a cursor that suddenly appears causes hover effects
            // (highlights, floating buttons) that must not be mistaken for
            // scrolling. 2026-11-14 lesson: this exact false positive made
            // the capture scroll down from the bottom of an already-bottom
            // page, where no wheel-down can ever move anything.
            movePointerTo(wheelPoint);
            pause(500);
            const QImage base = stableFrame(screen, region, SETTLE_TIMEOUT_MS);
            INPUT up{};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = MOUSEEVENTF_WHEEL;
            up.mi.mouseData = static_cast<DWORD>(WHEEL_DELTA);
            SendInput(1, &up, sizeof(INPUT));
            const QImage afterProbe =
              stableFrame(screen, region, SETTLE_TIMEOUT_MS);
            const RowProfile probePrevious = buildProfile(base);
            const RowProfile probeCurrent = buildProfile(afterProbe);
            const ColumnStats probeStats =
              analyseColumns(probePrevious, probeCurrent);
            const QVector<int> probeColumns = usableColumns(probeStats);
            const Region probeRegion{ 0, base.height() };
            const double moved = sameRowMad(
              probePrevious, probeCurrent, probeColumns, probeRegion);
            // A real scroll moves most of the region; a blinking caret or a
            // clock only perturbs a few rows. Require a decisive change.
            if (!base.isNull() && !afterProbe.isNull() && moved > 4.0) {
                // The page really scrolled, so it was not at the top. Wheel
                // all the way up instead of sending Ctrl+Home: custom scroll
                // containers (chat apps, editors) ignore it, and a stray Ctrl
                // modifier would turn the next wheel event into a browser
                // zoom.
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Content was scrolled, rewinding to the "
                                    "top with wheel notches");
                hud.setStatus(ZAG_TR("Rewinding to the top, long pages may take half a minute…"));
                QCoreApplication::processEvents();
                QImage last = afterProbe;
                int stillCount = 0;
                for (int i = 0; i < 60 && stillCount < 2; ++i) {
                    INPUT upN{};
                    upN.type = INPUT_MOUSE;
                    upN.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    upN.mi.mouseData = static_cast<DWORD>(WHEEL_DELTA * 3);
                    SendInput(1, &upN, sizeof(INPUT));
                    const QImage frame =
                      stableFrame(screen, region, SETTLE_TIMEOUT_MS);
                    if (frame.isNull() || frame.size() != last.size()) {
                        break;
                    }
                    const RowProfile a = buildProfile(last);
                    const RowProfile b = buildProfile(frame);
                    const ColumnStats st = analyseColumns(a, b);
                    const QVector<int> cols = usableColumns(st);
                    const double d =
                      sameRowMad(a, b, cols, Region{ 0, last.height() });
                    if (d <= MAD_IDENTICAL) {
                        ++stillCount;
                    } else {
                        stillCount = 0;
                    }
                    last = frame;
                }
                first = last;
                // The scroll position changed behind UI Automation's back;
                // refresh it so the bottom-of-document check stays truthful.
                if (uiaReady) {
                    const double home = uia.verticalPercent();
                    if (home >= 0.0) {
                        uiaPercent = home;
                    }
                }
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Capture starts at y=0 (wheel rewind)");
            } else if (!base.isNull()) {
                // One upward notch changed nothing: the page is already at
                // the top (or cannot scroll). Start from the settled frame
                // instead of the pre-hover one.
                first = base;
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Page is already at the top of the "
                                    "document");
            }
            // Either way the position is now known-good; refresh it so the
            // percent-based bottom check cannot fire on a stale value.
            if (uiaReady) {
                const double at = uia.verticalPercent();
                if (at >= 0.0) {
                    uiaPercent = at;
                }
            }
        }
    }

    const bool verbose = envFlag("FLAMESHOT_SCROLL_VERBOSE", false);
    int maxHeight = MAX_RESULT_HEIGHT;
    const QByteArray heightLimit = qgetenv("FLAMESHOT_SCROLL_MAX_HEIGHT");
    if (!heightLimit.isEmpty()) {
        maxHeight = qBound(1024, heightLimit.toInt(), MAX_RESULT_HEIGHT);
    }
    QElapsedTimer frameClock;
    qint64 msScroll = 0, msSettle = 0, msProfile = 0, msMatch = 0, msAppend = 0;
    // Starts at the conservative windows; the tuner itself decides when the
    // page has proven quiet enough to shrink them (see stableFrame).
    SettleTuner settleTuner;
    int maxFrames = MAX_FRAMES;
    const QByteArray frameLimit = qgetenv("FLAMESHOT_SCROLL_MAX_FRAMES");
    if (!frameLimit.isEmpty()) {
        maxFrames = qBound(1, frameLimit.toInt(), MAX_FRAMES);
    }
    const QString dumpDir =
      QString::fromLocal8Bit(qgetenv("FLAMESHOT_SCROLL_DUMP"));

    hud.setStatus(ZAG_TR("Preparing, press Esc to stop"));

    Canvas canvas;
    canvas.reset(first.width());
    if (!canvas.append(first, 0, first.height(), maxHeight)) {
        emit finished(false, QStringLiteral("could not allocate the canvas"));
        return;
    }

    QImage previous = first;
    RowProfile previousProfile = buildProfile(first);
    QStringList steps;
    QString stopReason = QStringLiteral("frame limit");
    int frames = 0;
    int lastShift = 0;
    int noChangeStreak = 0;
    int failedStreak = 0;
    int modesWithoutProgress = 0;
    int degradedSteps = 0;
    int footerHeight = 0;
    bool barsFrozen = false;
    int headerHeight = 0;
    // First frame captured after the content actually moved: comparing it with
    // `first` is what tells a fixed sidebar from a scrolling column.
    QImage bandProbe;

    for (; frames < maxFrames; ++frames) {
        if (escapePressed()) {
            stopReason = QStringLiteral("Esc pressed");
            break;
        }
        if (canvas.used >= maxHeight) {
            stopReason = QStringLiteral("height limit");
            break;
        }

        // Keep the HUD above the target window. The target was force-raised
        // before the loop and can also reclaim z-order on its own; without
        // this the progress text vanishes mid-capture ("滚着滚着交互界面消失").
        if (frames % 3 == 0) {
            hud.raise();
        }

        frameClock.start();
        // Bottom-of-document is a position fact, valid for both drivers — but
        // only after a real scroll attempt. Custom containers (chat apps)
        // report a stale position right after a programmatic rewind, and
        // trusting it on frame 0 aborted the capture before it produced a
        // single frame (2026-11-15 user report). Frame 0 always scrolls once
        // and lets the pixel evidence below decide.
        if (frames > 0 && uiaReady && uiaPercent >= 0.0 &&
            uiaPercent + uiaView >= 99.4) {
            stopReason = QStringLiteral("reached bottom");
            break;
        }
        if (uiaDrive) {
            // The container tells us where it is, so the step is exact and
            // never overshoots the end of the document.
            uia.scrollToPercent(qMin(100.0, uiaPercent + uiaStep));
            pause(70);
        } else {
            scrollDown(wheelPoint, target.hwnd, scrollMode, stepNotches);
        }
        msScroll += frameClock.restart();
        if (escapePressed()) {
            stopReason = QStringLiteral("Esc pressed");
            break;
        }

        QImage current = stableFrame(screen, region, SETTLE_TIMEOUT_MS, &settleTuner);
        msSettle += frameClock.restart();
        if (uiaReady) {
            const double now = uia.verticalPercent();
            if (now >= 0.0) {
                uiaPercent = now;
            }
        }
        if (current.isNull() || current.size() != previous.size()) {
            stopReason = QStringLiteral("frame grab failed or resized");
            break;
        }
        if (!dumpDir.isEmpty()) {
            current.save(QStringLiteral("%1/frame-%2.png")
                           .arg(dumpDir)
                           .arg(frames, 3, 10, QLatin1Char('0')),
                         "PNG");
        }

        RowProfile currentProfile = buildProfile(current);
        const ColumnStats stats = analyseColumns(previousProfile, currentProfile);
        const QVector<int> columns = usableColumns(stats);
        msProfile += frameClock.restart();

        // The bar layout is a property of the page, not of the frame pair.
        // Re-detecting it per frame lets a single spurious run (a static patch
        // at the bottom edge, for instance) move the append origin by a few
        // pixels and tear exactly that seam. Measure once, then freeze.
        if (!barsFrozen) {
            const Bars bars = detectFixedBars(
              previousProfile, currentProfile, columns, current.height());
            headerHeight = bars.top;
            footerHeight = bars.bottom;
            barsFrozen = true;
            if (footerHeight > 0 && canvas.used == first.height()) {
                // Those rows are already inside the first frame; drop them now
                // and write them back once at the very end.
                canvas.rewind(footerHeight);
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Fixed bottom bar detected: %1px (excluded)")
                       .arg(footerHeight);
            }
            if (headerHeight > 0) {
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Fixed top bar detected: %1px (excluded)")
                       .arg(headerHeight);
            }
        }

        const Region content{ headerHeight,
                              current.height() - footerHeight };
        if (content.height() < 32) {
            stopReason = QStringLiteral("scrollable area too small");
            break;
        }

        const double still =
          sameRowMad(previousProfile, currentProfile, columns, content);
        if (still <= MAD_IDENTICAL) {
            // Nothing moved. Either the input was swallowed by the target or
            // the document has genuinely reached its end; a different scroll
            // method distinguishes the two.
            ++noChangeStreak;
            failedStreak = 0;
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Step %1: no movement (mad=%2, mode=%3)")
                   .arg(frames)
                   .arg(still, 0, 'f', 2)
                   .arg(scrollMode);
            if (noChangeStreak >= 2) {
                noChangeStreak = 0;
                if (uiaDrive) {
                    // The provider advertised a scroll pattern but the pixels
                    // did not move - it may be driving a hidden container.
                    // Drop it and drive the wheel instead of giving up.
                    AbstractLogger::warning(AbstractLogger::LogFile)
                      << QStringLiteral("UI Automation scroll had no effect, "
                                        "falling back to wheel events");
                    uiaReady = false;
                    uiaDrive = false;
                    scrollMode = 0;
                    modesWithoutProgress = 0;
                } else {
                    ++modesWithoutProgress;
                    scrollMode = (scrollMode + 1) % MODE_COUNT;
                    if (modesWithoutProgress >= MODE_COUNT) {
                        stopReason = QStringLiteral("reached bottom");
                        break;
                    }
                }
            }
            previous = current;
            previousProfile = currentProfile;
            continue;
        }

        ShiftMatch match =
          findShift(previousProfile, currentProfile, columns, content, lastShift);
        msMatch += frameClock.restart();

        // A frame grabbed while the animation was still finishing measures a
        // short step. Retry the grab instead of stitching a squashed row.
        for (int retry = 0; retry < 2 && match.mad > MAD_GOOD; ++retry) {
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Step %1: weak match (mad=%2), re-grabbing (%3)")
                   .arg(frames)
                   .arg(match.mad, 0, 'f', 2)
                   .arg(retry + 1);
            pause(220);
            const QImage again = stableFrame(screen, region, SETTLE_TIMEOUT_MS);
            if (again.isNull() || again.size() != current.size()) {
                break;
            }
            current = again;
            // The profile must follow the frame, otherwise the next step
            // compares against a picture that is no longer `previous`.
            currentProfile = buildProfile(current);
            match =
              findShift(previousProfile, currentProfile, columns, content, lastShift);
        }

        if (verbose) {
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Step %1: dy=%2 mad=%3 rival=%4 amb=%5 "
                                "bars=%6/%7 cols=%8 mode=%9")
                   .arg(frames)
                   .arg(match.shift)
                   .arg(match.mad, 0, 'f', 2)
                   .arg(match.rival, 0, 'f', 2)
                   .arg(match.ambiguous ? 1 : 0)
                   .arg(headerHeight)
                   .arg(footerHeight)
                   .arg(columns.size())
                   .arg(scrollMode);
        }

        if (match.mad > MAD_ACCEPT || match.shift <= MIN_SHIFT) {
            ++failedStreak;
            noChangeStreak = 0;
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Step %1: match failed (mad=%2 dy=%3), attempt %4")
                   .arg(frames)
                   .arg(match.mad, 0, 'f', 2)
                   .arg(match.shift)
                   .arg(failedStreak);
            if (failedStreak >= 3) {
                stopReason = QStringLiteral("content could not be matched");
                break;
            }
            if (failedStreak == 2) {
                scrollMode = (scrollMode + 1) % MODE_COUNT;
            }
            previous = current;
            previousProfile = currentProfile;
            continue;
        }

        const int dy = match.shift;
        if (!match.confident()) {
            ++degradedSteps;
            AbstractLogger::warning(AbstractLogger::LogFile)
              << QStringLiteral("Step %1: low confidence match dy=%2 mad=%3 "
                                "rival=%4")
                   .arg(frames)
                   .arg(dy)
                   .arg(match.mad, 0, 'f', 2)
                   .arg(match.rival, 0, 'f', 2);
        }

        noChangeStreak = 0;
        failedStreak = 0;
        modesWithoutProgress = 0;
        lastShift = dy;
        // Keep one step inside what the matcher can verify confidently: each
        // notch moves 100-150 logical px, so a huge dy means zoomed content
        // and fewer notches per step stitch far more reliably.
        if (dy > 450 && stepNotches > 1) {
            stepNotches = qMax(1, stepNotches / 2);
            AbstractLogger::info(AbstractLogger::LogFile)
              << QStringLiteral("Large scroll step (dy=%1 px), reducing to "
                                "%2 notch(es)")
                   .arg(dy)
                   .arg(stepNotches);
        } else if (dy < 120 && stepNotches < notches) {
            ++stepNotches;
        }
        hud.setStatus(ZAG_TR("Stitched %1 px · frame %2 · Esc to stop")
                        .arg(canvas.used + dy)
                        .arg(frames + 1));
        QCoreApplication::processEvents();
        steps << QStringLiteral("%1@%2").arg(dy).arg(match.mad, 0, 'f', 1);

        // Fixed footer rows must not be appended with the scrolling content,
        // otherwise the same bar would be repeated on every seam.
        const int stripTop = content.bottom - dy;
        if (stripTop < content.top) {
            stopReason = QStringLiteral("overlap exhausted");
            break;
        }
        if (!canvas.append(current, stripTop, dy, maxHeight)) {
            stopReason = QStringLiteral("height limit");
            break;
        }
        if (bandProbe.isNull()) {
            bandProbe = current;
        }
        msAppend += frameClock.restart();
        previous = current;
        previousProfile = currentProfile;
    }

    // Write the fixed footer once, at the very bottom.
    if (footerHeight > 0 && !previous.isNull() &&
        previous.height() >= footerHeight) {
        canvas.append(previous, previous.height() - footerHeight, footerHeight,
                      maxHeight);
    }

    // The scrolling is over; drop the outline before the result badge shows.
    outline.reset();
    QCoreApplication::processEvents();

    if (frames == 0) {
        // Not a single usable step. Delivering the settled top frame is still
        // far better than nothing: "the area does not scroll" is a legitimate
        // outcome, and a static screenshot answers it (2026-11-15: aborting
        // with "no image written" after a minute of work reads as broken).
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Scrolling capture: the selection did not scroll "
                            "(%1), saving the static frame instead")
               .arg(stopReason);
        const QString path = exportResult(first);
        if (!path.isEmpty()) {
            hud.setBadge(QColor(239, 68, 68)); // red: nothing scrolled
            hud.setTitle(ZAG_TR("Selection cannot scroll"));
            hud.setStatus(ZAG_TR("Static screenshot saved"));
            pause(1500);
            emit finished(true, path);
            return;
        }
        hud.setBadge(QColor(239, 68, 68));
        hud.setTitle(ZAG_TR("Scrolling capture incomplete"));
        hud.setStatus(ZAG_TR("The selection did not scroll, try another content area"));
        pause(1800);
        emit finished(false,
                      QStringLiteral("the selection did not scroll: %1")
                        .arg(stopReason));
        return;
    }

    QImage result = canvas.result();
    if (result.isNull()) {
        emit finished(false, QStringLiteral("nothing was captured"));
        return;
    }

    // Drop a fixed side panel (chat list / navigation tree) once, instead of
    // letting it repeat down the whole long image. Cropping the assembled
    // canvas is safe here because the strip is identical in every frame.
    QString sideNote;
    if (!bandProbe.isNull()) {
        const SideBands bands = detectStaticSideBands(first, bandProbe);
        if (bands.width() > 0) {
            const QRect keep(bands.left,
                             0,
                             result.width() - bands.width(),
                             result.height());
            if (keep.width() >= 120) {
                result = result.copy(keep);
                sideNote = QStringLiteral(" ") + ZAG_TR("· fixed sidebar removed");
                AbstractLogger::info(AbstractLogger::LogFile)
                  << QStringLiteral("Static side panel removed: left=%1px "
                                    "right=%2px, result width %3 px")
                       .arg(bands.left)
                       .arg(bands.right)
                       .arg(result.width());
            }
        }
    }

    if (verbose) {
        AbstractLogger::info(AbstractLogger::LogFile)
          << QStringLiteral("Timing ms: scroll=%1 settle=%2 profile=%3 "
                            "match=%4 append=%5 frames=%6 avg=%7")
               .arg(msScroll)
               .arg(msSettle)
               .arg(msProfile)
               .arg(msMatch)
               .arg(msAppend)
               .arg(frames)
               .arg(frames > 0 ? (msScroll + msSettle + msProfile + msMatch +
                                 msAppend) / frames : 0)
          << QStringLiteral("  grabs=%1 grabMs=%2 avgGrab=%3")
               .arg(g_grabCount)
               .arg(g_grabMs)
               .arg(g_grabCount > 0 ? g_grabMs / g_grabCount : 0);
    }
    AbstractLogger::info(AbstractLogger::LogFile)
      << QStringLiteral("Frames=%1 dy=[%2] bars=top%3/bottom%4 degraded=%5 "
                        "stopped=%6 driver=%7")
             .arg(frames)
             .arg(steps.join(QStringLiteral(",")))
             .arg(headerHeight)
             .arg(footerHeight)
             .arg(degradedSteps)
             .arg(stopReason)
             .arg(uiaDrive ? QStringLiteral("uia")
                           : QStringLiteral("wheel"));

    const QString path = exportResult(result);
    const bool ok = !path.isEmpty();
    // Reaching the height/frame cap is not a failure, but it is not a full
    // page either: the user must know the image stops before the document
    // ends, or the next report is "少了最后几屏".
    const bool truncated =
      stopReason == QLatin1String("height limit") ||
      stopReason == QLatin1String("frame limit");
    if (ok) {
        // The editor is gone by now, so this HUD is the only feedback the user
        // gets. Without it a successful capture looks like nothing happened.
        hud.setBadge(degradedSteps > 0 || truncated
                       ? QColor(234, 179, 8)   // yellow
                       : QColor(34, 197, 94)); // green
        hud.setTitle(ZAG_TR("Scrolling capture complete"));
        QString status = ZAG_TR("Saved and copied to clipboard") + sideNote;
        if (truncated) {
            status += QStringLiteral(" ") + ZAG_TR("· Limit reached, remaining page not captured");
        }
        hud.setStatus(status);
        pause(1500);
    }
    const QString quality = degradedSteps > 0 ? QStringLiteral("degraded")
                                              : QStringLiteral("clean");
    const QString detail =
      QStringLiteral("%1 (%2x%3, %4 frames, %5)")
        .arg(ok ? path : QStringLiteral("save failed"))
        .arg(result.width())
        .arg(result.height())
        .arg(frames)
        .arg(quality);

    AbstractLogger::info(AbstractLogger::LogFile | AbstractLogger::LogFile)
      << QStringLiteral("Scrolling capture: ") + detail +
           QStringLiteral(", stopped: ") + stopReason + QStringLiteral("\n");
    emit finished(ok, detail);
}

// ---------------------------------------------------------------------------
// `flameshot scroll --probe`
//
// Runs the *shipping* target detection - window voting plus UI Automation
// lookup - over every visible window and prints the result. Nothing is
// scrolled, no foreground is taken and no pixels are grabbed, so it is safe to
// run while the desktop is in use and it answers the only question that
// matters when a capture "does not work": was a scrollable area found at all?
// ---------------------------------------------------------------------------
namespace {

QScreen* screenForPhysicalPoint(const QPoint& physical)
{
    for (QScreen* screen : QGuiApplication::screens()) {
        const RECT monitor = monitorPhysicalRect(screen->name());
        if (physical.x() >= monitor.left && physical.x() < monitor.right &&
            physical.y() >= monitor.top && physical.y() < monitor.bottom) {
            return screen;
        }
    }
    return QGuiApplication::primaryScreen();
}

BOOL CALLBACK diagnoseEnumProc(HWND window, LPARAM data)
{
    auto* out = reinterpret_cast<QVector<HWND>*>(data);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    wchar_t title[256] = {};
    if (GetWindowTextW(window, title, 255) == 0) {
        return TRUE;
    }
    RECT frame{};
    if (!GetWindowRect(window, &frame)) {
        return TRUE;
    }
    if (frame.right - frame.left < 200 || frame.bottom - frame.top < 200) {
        return TRUE;
    }
    if (isDesktopShell(window)) {
        return TRUE;
    }
    out->append(window);
    return TRUE;
}

} // namespace

void ScrollCapture::diagnose()
{
    QVector<HWND> windows;
    EnumWindows(diagnoseEnumProc, reinterpret_cast<LPARAM>(&windows));
    printf("visible top-level windows: %d\n",
           static_cast<int>(windows.size()));

    for (HWND window : windows) {
        RECT frame{};
        GetWindowRect(window, &frame);
        wchar_t title[256] = {};
        GetWindowTextW(window, title, 255);
        wchar_t cls[128] = {};
        GetClassNameW(window, cls, 127);

        const QPoint center((frame.left + frame.right) / 2,
                            (frame.top + frame.bottom) / 2);
        QScreen* screen = screenForPhysicalPoint(center);
        if (screen == nullptr) {
            continue;
        }
        const QRect logical = logicalFromPhysical(screen, frame);
        // The inset a selection drawn inside the window would have.
        QRect region = logical.adjusted(logical.width() / 10,
                                        logical.height() / 10,
                                        -logical.width() / 10,
                                        -logical.height() / 10);
        if (region.width() < MIN_REGION_SIZE ||
            region.height() < MIN_REGION_SIZE) {
            region = logical;
        }

        const ScrollTarget target = resolveScrollTarget(screen, region);
        QRect content = region.intersected(target.logical);
        if (content.width() < MIN_REGION_SIZE ||
            content.height() < MIN_REGION_SIZE) {
            content = region;
        }
        content.adjust(2, 2, -2, -2);
        const int guard = qMin(20, qMax(0, content.width() / 3));
        content.adjust(0, 0, -guard, 0);

        ScrollUia uia;
        QPoint hit;
        const bool found = attachScrollContainer(uia, screen, content, &hit);
        QPoint wheel = physicalCenter(screen, content);
        if (found) {
            const QRect container =
              logicalFromPhysicalRect(screen, uia.boundsPhysical());
            if (container.width() >= MIN_REGION_SIZE &&
                container.height() >= MIN_REGION_SIZE &&
                container.intersects(content)) {
                wheel = physicalCenter(screen, container.intersected(content));
            }
        }

        const QString targetText =
          target.valid()
            ? QStringLiteral("target=\"%1\" votes=%2")
                .arg(target.title)
                .arg(target.votes)
            : QStringLiteral("target=<none>");
        const QString uiaText =
          found
            ? QStringLiteral("%1 probe=(%2,%3) view=%4% pos=%5% "
                             "wheel-point=(%6,%7)")
                .arg(uia.description())
                .arg(hit.x())
                .arg(hit.y())
                .arg(uia.viewPercent(), 0, 'f', 1)
                .arg(uia.verticalPercent(), 0, 'f', 1)
                .arg(wheel.x())
                .arg(wheel.y())
            : QStringLiteral("<no scroll container>");

        printf("WINDOW \"%s\" [%s] %ldx%ld\n    %s | %s\n",
               qUtf8Printable(QString::fromWCharArray(title)),
               qUtf8Printable(QString::fromWCharArray(cls)),
               frame.right - frame.left,
               frame.bottom - frame.top,
               qUtf8Printable(targetText),
               qUtf8Printable(uiaText));
        fflush(stdout);
    }
}

#ifdef ENABLE_SELFTEST
namespace {

int countPixelsNear(const QImage& image, const QColor& color, int tolerance)
{
    int hits = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() < 200) {
                continue;
            }
            if (qAbs(pixel.red() - color.red()) <= tolerance &&
                qAbs(pixel.green() - color.green()) <= tolerance &&
                qAbs(pixel.blue() - color.blue()) <= tolerance) {
                ++hits;
            }
        }
    }
    return hits;
}

} // namespace

bool ScrollCapture::pickerSelfTest(QString& report)
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        report = QStringLiteral("picker: no screen");
        return false;
    }
    const QRect screenRect = screen->geometry();

    // A synthetic background: the overlay must be verifiable without reading a
    // single pixel of the user's desktop.
    QPixmap background(screenRect.size());
    background.fill(QColor(40, 44, 52));

    const QRect initial(
      screenRect.x() + 200, screenRect.y() + 150, 480, 360);
    const QRect detected = initial.adjusted(-40, -40, 60, 60);

    ScrollRegionPicker picker(screen,
                              initial,
                              detected,
                              QStringLiteral("Group \"对话\" (view 22.3%)"),
                              background);
    picker.setAttribute(Qt::WA_DontShowOnScreen, true);
    picker.show();
    QCoreApplication::processEvents();

    auto sendKey = [&picker](int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(&picker, &event);
    };

    for (int i = 0; i < 5; ++i) {
        sendKey(Qt::Key_Right, Qt::NoModifier);
    }
    for (int i = 0; i < 3; ++i) {
        sendKey(Qt::Key_Down, Qt::NoModifier);
    }
    const QRect moved = picker.selected();
    const bool moveOk = moved.topLeft() == initial.topLeft() + QPoint(5, 3) &&
                        moved.size() == initial.size();

    for (int i = 0; i < 10; ++i) {
        sendKey(Qt::Key_Right, Qt::ControlModifier);
    }
    for (int i = 0; i < 5; ++i) {
        sendKey(Qt::Key_Down, Qt::ControlModifier);
    }
    const QRect resized = picker.selected();
    const bool resizeOk = resized.width() == initial.width() + 10 &&
                          resized.height() == initial.height() + 5;

    sendKey(Qt::Key_Return, Qt::NoModifier);
    const bool confirmedOk = picker.confirmed();

    QImage rendered(picker.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    {
        QPainter painter(&rendered);
        picker.render(&painter);
    }
    const int accentPixels =
      countPixelsNear(rendered, QColor(56, 189, 248), 12);
    const bool paintedOk = accentPixels > 800;

    // Esc must abort, not confirm.
    ScrollRegionPicker cancelling(
      screen, initial, detected, QStringLiteral("x"), background);
    cancelling.setAttribute(Qt::WA_DontShowOnScreen, true);
    cancelling.show();
    QCoreApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&cancelling, &escape);
    const bool cancelOk = !cancelling.confirmed();

    picker.hide();
    cancelling.hide();

    report = QStringLiteral(
               "picker: initial=(%1,%2 %3x%4) moved=(%5,%6) resized=%7x%8 "
               "confirm=%9 cancel=%10 accent-pixels=%11")
               .arg(initial.x())
               .arg(initial.y())
               .arg(initial.width())
               .arg(initial.height())
               .arg(moved.x())
               .arg(moved.y())
               .arg(resized.width())
               .arg(resized.height())
               .arg(confirmedOk ? "yes" : "no")
               .arg(cancelOk ? "yes" : "no")
               .arg(accentPixels);

    return moveOk && resizeOk && confirmedOk && paintedOk && cancelOk;
}
#endif
