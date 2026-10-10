#pragma once
#include <QRect>

// 网格与自由吸附共用的放置轮廓及淡出动画。
namespace GridPlacementOverlay {
void display(const QRect& target);
void dismiss();
}
