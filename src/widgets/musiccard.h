#pragma once

#include "media/systemmediasession.h"
#include <QRectF>
class QPainter;

namespace MusicCard {
// 绘制与命中检测共用同一组区域，避免缩放后按钮点击位置偏移。
struct Layout {
    QRectF artwork;
    QRectF title;
    QRectF artist;
    QRectF status;
    QRectF progress;
    QRectF previous;
    QRectF toggle;
    QRectF next;
};
Layout layout(const QRectF& bounds);
void paint(QPainter& painter, const QRectF& bounds, const MediaSnapshot& media);
}
