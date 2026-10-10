#include "widgets/desktopwidget.h"
#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"
#include "app/appsettings.h"
#include "ui/gridplacementoverlay.h"
#include "ui/widgetlibrarydialog.h"
#include <QGuiApplication>
#include <QScreen>
#include <QHash>
#include <QToolTip>
#include <QtMath>
#include <limits>

using namespace CardMetrics;
using namespace WidgetGeometry;

void DesktopWidget::applyScale(qreal scale)
{
    m_widgetScale = qBound<qreal>(0.40, scale, 2.0);
    AppSettings settings;
    settings.setValue(QStringLiteral("appearance/scale"), m_widgetScale);
    if (m_library) {
        settings.sync();
        m_library->updateScreenLayout();
    }
    setGlassRadius(LiquidGlassStyle::kCardCornerRadius * m_widgetScale);
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    QScreen* currentScreen = QGuiApplication::screenAt(geometry().center());
    if (!currentScreen)
        currentScreen = QGuiApplication::primaryScreen();
    const int gap = currentScreen
        ? gridGapForArea(currentScreen->availableGeometry(), m_widgetScale)
        : qMax(4, qRound(kGridBaseGap * m_widgetScale));
    setFixedSize(gridWidgetSize(m_cardKind, m_cardVariant, cell, gap));
    if (isVisible() && overlapsVisibleWidget(geometry(), this)) {
        const QRect target = nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this);
        if (target.isValid()) {
            m_restoringPosition = true;
            setFixedSize(target.size());
            move(target.topLeft());
            m_restoringPosition = false;
        }
    }
    updateCardGeometry();
    captureDesktopBackdrop();
    update();
}

void DesktopWidget::updateCardGeometry()
{
    // 每个窗口仅包含一个完整玻璃对象；基类管理圆角与背景，本类负责选定卡片内容。
    setGlassObjectVisible(0, true);
    setGlassObjectGeometry(0, QPointF(0, 0), QSizeF(width(), height()));
    setGlassObjectCornerRadius(0, float(LiquidGlassStyle::kCardCornerRadius * m_widgetScale));
}

void DesktopWidget::moveToGroupPosition()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    const int gap = gridGapForArea(area, m_widgetScale);
    const int pitch = cell + gap;
    // 扣除两侧外边距后计算网格容量，同时保留末端一份间隙。
    const int columns = qMax(1, (area.width() - gap) / pitch);
    const int rows = qMax(1, (area.height() - gap) / pitch);
    const int span = gridColumnSpan(m_cardKind, m_cardVariant);
    const int column = qMax(0, columns - span);
    const int rowSpan = gridRowSpan(m_cardKind, m_cardVariant);
    const int row = qMax(0, rows - rowSpan);
    const QRect target = nearestGridRect(m_cardKind, m_cardVariant,
        gridOrigin(area, gap) + QPoint(column * pitch, row * pitch), m_widgetScale, this);
    if (target.isValid()) {
        m_restoringPosition = true;
        setFixedSize(target.size());
        move(target.topLeft());
        m_restoringPosition = false;
    }
}

void DesktopWidget::arrangeGroup()
{
    // 先腾空旧位置，避免组件自身的旧布局阻挡重新排列。
    int offset = 0;
    QHash<DesktopWidget*, QPoint> previous;
    for (DesktopWidget* widget : s_instances) {
        previous.insert(widget, widget->pos());
        widget->m_restoringPosition = true;
        widget->move(-100000 - offset, -100000 - offset);
        widget->m_restoringPosition = false;
        offset += 8;
    }
    for (DesktopWidget* widget : s_instances) {
        widget->moveToGroupPosition();
        if (!QGuiApplication::screenAt(widget->geometry().center())) {
            // 网格已满时恢复原位置，不能将卡片留在屏幕外的暂存坐标。
            for (DesktopWidget* restore : s_instances)
                restore->move(previous.value(restore));
            QToolTip::showText(QCursor::pos(), QStringLiteral("当前屏幕空间不足，请缩小组件后重新排列"));
            break;
        }
        widget->captureDesktopBackdrop();
    }
}

QRect DesktopWidget::nearestGridRect(CardKind kind, int variant,
                                     const QPoint& requestedTopLeft, qreal scale,
                                     const DesktopWidget* ignored)
{
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int columnSpan = gridColumnSpan(kind, variant);
    const QSize roughSize(cell * columnSpan, cell);
    QScreen* screen = QGuiApplication::screenAt(
        requestedTopLeft + QPoint(roughSize.width() / 2, roughSize.height() / 2));
    if (!screen)
        screen = QGuiApplication::screenAt(requestedTopLeft);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QRect(requestedTopLeft, roughSize);

    const QRect area = screen->availableGeometry();
    const int gap = gridGapForArea(area, scale);
    const QSize widgetSize = gridWidgetSize(kind, variant, cell, gap);
    const int pitch = cell + gap;
    // 所有外边缘都预留一份网格间隙，候选矩形的右侧与底部也必须满足。
    const int innerWidth = area.width() - gap * 2;
    const int innerHeight = area.height() - gap * 2;
    if (innerWidth < widgetSize.width() || innerHeight < widgetSize.height())
        return {};
    const int maxColumn = qMax(0, (innerWidth - widgetSize.width()) / pitch);
    const int maxRow = qMax(0, (innerHeight - widgetSize.height()) / pitch);
    const QPoint origin = gridOrigin(area, gap);
    QRect best;
    qint64 bestDistance = (std::numeric_limits<qint64>::max)();

    for (int row = 0; row <= maxRow; ++row) {
        for (int column = 0; column <= maxColumn; ++column) {
            const QRect candidate(origin.x() + column * pitch,
                                  origin.y() + row * pitch,
                                  widgetSize.width(), widgetSize.height());
            // 隐藏卡片保留恢复位置，但不占用可见桌面网格。
            if (overlapsVisibleWidget(candidate, ignored))
                continue;

            const qint64 dx = qint64(candidate.left()) - requestedTopLeft.x();
            const qint64 dy = qint64(candidate.top()) - requestedTopLeft.y();
            const qint64 distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
    }

    if (!best.isNull())
        return best;

    // 无可用位置时返回无效矩形，拖动退回原位，图库拒绝创建。
    return QRect();
}

QRect DesktopWidget::nearbyWidgetRect(CardKind kind, int variant,
                                      const QPoint& requestedTopLeft, qreal scale,
                                      const DesktopWidget* ignored)
{
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    QScreen* screen = QGuiApplication::screenAt(requestedTopLeft);
    if (!screen)
        screen = QGuiApplication::screenAt(requestedTopLeft + QPoint(cell / 2, cell / 2));
    if (!screen)
        return {};

    const QRect area = screen->availableGeometry();
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, variant, cell, gap);
    const QRect requested(requestedTopLeft, size);
    const bool overlapping = overlapsVisibleWidget(requested, ignored);
    // 检测整条边与四角附近的吸附位置，提示轮廓保持固定对齐槽位。
    const int threshold = qMax(96, qRound(180 * scale));
    qint64 bestDistance = (std::numeric_limits<qint64>::max)();
    QRect best;

    for (DesktopWidget* neighbor : s_instances) {
        if (!neighbor || neighbor == ignored || !neighbor->isVisible()
            || QGuiApplication::screenAt(neighbor->geometry().center()) != screen)
            continue;
        const QRect beside = neighbor->geometry();
        const int verticalOverlap = qMax(0, qMin(requested.bottom(), beside.bottom())
                                          - qMax(requested.top(), beside.top()) + 1);
        const int horizontalOverlap = qMax(0, qMin(requested.right(), beside.right())
                                            - qMax(requested.left(), beside.left()) + 1);
        const bool besideEdge = verticalOverlap >= qMax(1, qMin(size.height(), beside.height()) / 4);
        const bool aboveOrBelow = horizontalOverlap >= qMax(1, qMin(size.width(), beside.width()) / 4);
        const QPoint candidates[] = {
            {beside.left() - gap - size.width(), beside.top()},
            {beside.left() - gap - size.width(), beside.bottom() - size.height() + 1},
            {beside.right() + gap + 1, beside.top()},
            {beside.right() + gap + 1, beside.bottom() - size.height() + 1},
            {beside.left(), beside.top() - gap - size.height()},
            {beside.right() - size.width() + 1, beside.top() - gap - size.height()},
            {beside.left(), beside.bottom() + gap + 1},
            {beside.right() - size.width() + 1, beside.bottom() + gap + 1},
            {beside.left() - gap - size.width(), beside.top() - gap - size.height()},
            {beside.right() + gap + 1, beside.top() - gap - size.height()},
            {beside.left() - gap - size.width(), beside.bottom() + gap + 1},
            {beside.right() + gap + 1, beside.bottom() + gap + 1}
        };
        const auto consider = [&](const QPoint& position, int index) {
            if (!overlapping) {
                const int dx = qAbs(position.x() - requestedTopLeft.x());
                const int dy = qAbs(position.y() - requestedTopLeft.y());
                if (index < 4) {
                    if (!besideEdge || dx > threshold)
                        return;
                } else if (index < 8) {
                    if (!aboveOrBelow || dy > threshold)
                        return;
                } else if (besideEdge || aboveOrBelow
                           || dx > threshold || dy > threshold) {
                    return;
                }
            }
            const QRect candidate(position, size);
            if (!area.contains(candidate))
                return;
            if (overlapsVisibleWidget(candidate, ignored))
                return;
            const qint64 dx = qint64(position.x()) - requestedTopLeft.x();
            const qint64 dy = qint64(position.y()) - requestedTopLeft.y();
            const qint64 distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        };
        for (int index = 0; index < 12; ++index)
            consider(candidates[index], index);

        // 大卡片的边缘按单格步进，允许半幅交错，候选仍须通过边界和碰撞检查。
        const int pitch = cell + gap;
        for (int row = 1 - gridRowSpan(kind, variant);
             row < gridRowSpan(neighbor->m_cardKind, neighbor->m_cardVariant); ++row) {
            const int y = beside.top() + row * pitch;
            consider(QPoint(beside.left() - gap - size.width(), y), 0);
            consider(QPoint(beside.right() + gap + 1, y), 0);
        }
        for (int column = 1 - gridColumnSpan(kind, variant);
             column < gridColumnSpan(neighbor->m_cardKind, neighbor->m_cardVariant); ++column) {
            const int x = beside.left() + column * pitch;
            consider(QPoint(x, beside.top() - gap - size.height()), 4);
            consider(QPoint(x, beside.bottom() + gap + 1), 4);
        }
    }
    if (best.isValid())
        return best;
    // 目标位置冲突时尝试空闲网格，不能把组件提交到其他卡片上方。
    return overlapping ? nearestGridRect(kind, variant, requestedTopLeft, scale, ignored)
                       : QRect();
}

bool DesktopWidget::overlapsVisibleWidget(const QRect& rect, const DesktopWidget* ignored)
{
    for (DesktopWidget* widget : s_instances) {
        if (widget && widget != ignored && widget->isVisible()
            && rect.intersects(widget->geometry()))
            return true;
    }
    return false;
}

void DesktopWidget::showGridPreview(CardKind kind, int variant, const QPoint& desktopPoint)
{
    AppSettings settings;
    const qreal scale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    QScreen* screen = QGuiApplication::screenAt(desktopPoint);
    if (!screen) {
        hideGridPreview();
        return;
    }
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, variant, cell, gap);
    if (size.width() > area.width() || size.height() > area.height()) {
        hideGridPreview();
        return;
    }
    const QPoint desired = desktopPoint - QPoint(size.width() / 2, size.height() / 2);
    const QPoint freePosition(qBound(area.left(), desired.x(), area.right() - size.width() + 1),
                              qBound(area.top(), desired.y(), area.bottom() - size.height() + 1));
    showGridPreviewRect(nearbyWidgetRect(kind, variant, freePosition, scale));
}

void DesktopWidget::showGridPreviewRect(const QRect& rect)
{
    if (!rect.isValid()) {
        hideGridPreview();
        return;
    }
    GridPlacementOverlay::display(rect);
}

void DesktopWidget::hideGridPreview()
{
    GridPlacementOverlay::dismiss();
}

bool DesktopWidget::placeAtDesktopPoint(const QPoint& point)
{
    QScreen* screen = QGuiApplication::screenAt(point);
    if (!screen)
        return false;
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    const QSize size = gridWidgetSize(m_cardKind, m_cardVariant, cell,
                                      gridGapForArea(area, m_widgetScale));
    if (size.width() > area.width() || size.height() > area.height())
        return false;
    const QPoint desired = point - QPoint(size.width() / 2, size.height() / 2);
    const QPoint freePosition(qBound(area.left(), desired.x(), area.right() - size.width() + 1),
                              qBound(area.top(), desired.y(), area.bottom() - size.height() + 1));
    const QRect snapped = nearbyWidgetRect(m_cardKind, m_cardVariant, freePosition, m_widgetScale, this);
    if (!snapped.isValid() && overlapsVisibleWidget(QRect(freePosition, size), this))
        return false;
    const QRect target = snapped.isValid() ? snapped : QRect(freePosition, size);
    m_restoringPosition = true;
    setFixedSize(target.size());
    move(target.topLeft());
    m_restoringPosition = false;
    m_hasSavedPosition = true;
    captureDesktopBackdrop();
    saveAllConfigurations();
    return true;
}

QSize DesktopWidget::cardSize(CardKind kind, int variant, qreal scale, const QRect& workArea)
{
    scale = qBound<qreal>(.4, scale, 2.0);
    return gridWidgetSize(kind, variant, qMax(1, qRound(kGridBaseCell * scale)),
                          gridGapForArea(workArea, scale));
}

