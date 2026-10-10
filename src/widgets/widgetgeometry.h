#pragma once
#include "widgets/desktopwidget.h"

// 卡片类型、变体与桌面网格之间的统一换算，宿主、预览和恢复逻辑共用。
namespace WidgetGeometry {
int defaultVariant(DesktopWidget::CardKind kind);
int variantCount(DesktopWidget::CardKind kind);
int normalizedVariant(DesktopWidget::CardKind kind, int variant);
int gridColumnSpan(DesktopWidget::CardKind kind, int variant);
int gridRowSpan(DesktopWidget::CardKind kind, int variant);
int gridGapForArea(const QRect& area, qreal scale);
QSize gridWidgetSize(DesktopWidget::CardKind kind, int variant, int cell, int gap);
QPoint gridOrigin(const QRect& area, int gap);
}
