#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"
#include <QtMath>

using namespace CardMetrics;

namespace WidgetGeometry {
int defaultVariant(DesktopWidget::CardKind kind)
{
    switch (kind) {
    case DesktopWidget::CardKind::Battery: return 1;    // 横向状态。
    case DesktopWidget::CardKind::Weather: return 0;    // 紧凑预报。
    case DesktopWidget::CardKind::Clock: return 0;      // 浅色指针表盘。
    case DesktopWidget::CardKind::Dictionary: return 1; // 横向释义。
    case DesktopWidget::CardKind::Music: return 1;
    case DesktopWidget::CardKind::Countdown: return 0;
    case DesktopWidget::CardKind::Custom: return 1;
    }
    return 0;
}

int variantCount(DesktopWidget::CardKind kind)
{
    return kind == DesktopWidget::CardKind::Clock ? 9
        : kind == DesktopWidget::CardKind::Music || kind == DesktopWidget::CardKind::Countdown ? 2 : 3;
}

int normalizedVariant(DesktopWidget::CardKind kind, int variant)
{
    if (variant < 0)
        return defaultVariant(kind);
    return qBound(0, variant, variantCount(kind) - 1);
}

int gridColumnSpan(DesktopWidget::CardKind kind, int variant)
{
    variant = normalizedVariant(kind, variant);
    if (kind == DesktopWidget::CardKind::Clock)
        return variant == 7 || variant == 8 ? 2 : 1;
    if (kind == DesktopWidget::CardKind::Battery || kind == DesktopWidget::CardKind::Weather)
        return variant == 1 || variant == 2 ? 2 : 1;
    if (kind == DesktopWidget::CardKind::Custom)
        return variant == 0 ? 1 : 2;
    return variant == 1 ? 2 : 1;
}

int gridRowSpan(DesktopWidget::CardKind kind, int variant)
{
    const int normalized = normalizedVariant(kind, variant);
    if (kind == DesktopWidget::CardKind::Clock)
        return 1;
    return ((kind == DesktopWidget::CardKind::Battery
             || kind == DesktopWidget::CardKind::Weather || kind == DesktopWidget::CardKind::Custom)
            && normalized == 2) ? 2 : 1;
}

int gridGapForArea(const QRect& area, qreal scale)
{
    if (!area.isValid())
        return qMax(4, qRound(kGridBaseGap * scale));

    const qreal shortestEdge = qreal(qMin(area.width(), area.height()));
    const qreal screenFactor = qBound<qreal>(0.72, shortestEdge / 1080.0, 1.65);
    return qBound(4, qRound(kGridBaseGap * scale * screenFactor), 28);
}

QSize gridWidgetSize(DesktopWidget::CardKind kind, int variant, int cell, int gap)
{
    const int span = gridColumnSpan(kind, variant);
    const int rows = gridRowSpan(kind, variant);
    return QSize(cell * span + gap * (span - 1),
                 cell * rows + gap * (rows - 1));
}

QPoint gridOrigin(const QRect& area, int gap)
{
    // 外侧留白与相邻网格间隙保持一致。
    return area.topLeft() + QPoint(gap, gap);
}


}
