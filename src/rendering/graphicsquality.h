#pragma once

#include <QCache>
#include <QImage>
#include <QPainter>
#include <QtMath>
#include <utility>

namespace GraphicsQuality {
// 绘制作用域独立于文字模式，嵌套预览结束后恢复宿主的图像设置。
inline thread_local bool active = true;
class Scope final {
public:
    explicit Scope(bool enabled) : previous(active) { active = enabled; }
    ~Scope() { active = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    bool previous;
};
inline bool enabled() { return active; }

inline void drawImage(QPainter& painter, const QRectF& target, const QImage& image,
                      QRectF source = {})
{
    if (image.isNull() || target.isEmpty())
        return;
    if (source.isEmpty())
        source = QRectF(image.rect());
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, enabled());
    const QTransform transform = painter.deviceTransform();
    const QSizeF pixels = transform.mapRect(target).size();
    if (enabled() && transform.type() <= QTransform::TxScale
        && pixels.width() > 0 && pixels.height() > 0
        && (source.width() > pixels.width() || source.height() > pixels.height())) {
        // 先按目标物理像素做面积缩小，避免 GPU 双线性采样漏掉细线和透明边缘。
        const QSize size(qMax(1, qCeil(image.width() * pixels.width() / source.width())),
                         qMax(1, qCeil(image.height() * pixels.height() / source.height())));
        const QString key = QStringLiteral("%1:%2x%3")
            .arg(image.cacheKey()).arg(size.width()).arg(size.height());
        static thread_local QCache<QString, QImage> cache(16 * 1024);
        QImage* scaled = cache.object(key);
        if (!scaled) {
            QImage reduced = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            const int cost = qMax(1, int((reduced.sizeInBytes() + 1023) / 1024));
            if (cost <= cache.maxCost()) {
                scaled = new QImage(std::move(reduced));
                cache.insert(key, scaled, cost);
            }
        }
        if (scaled) {
            const qreal sx = scaled->width() / qreal(image.width());
            const qreal sy = scaled->height() / qreal(image.height());
            painter.drawImage(target, *scaled, QRectF(source.x() * sx, source.y() * sy,
                                                     source.width() * sx, source.height() * sy));
            painter.restore();
            return;
        }
    }
    painter.drawImage(target, image, source);
    painter.restore();
}
}
