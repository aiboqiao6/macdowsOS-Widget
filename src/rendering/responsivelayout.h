#pragma once

#include <QFontMetricsF>
#include <QCache>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtMath>
#include <memory>
#include "rendering/cleartypetext.h"

namespace ResponsiveLayout {

// 按最终设备像素栅格化文字并保留字形微调，字体缓存分辨率独立于背景画质。
inline bool drawHighQualityText(QPainter& painter, const QPointF& baseline,
                                const QString& value)
{
    return ClearTypeText::draw(painter, baseline, value);
}

class Metrics
{
public:
    explicit Metrics(const QRectF& bounds)
        : m_bounds(bounds), m_unit(qMax<qreal>(1.0,
                   qMin(bounds.width(), bounds.height())))
    {
    }

    qreal unit() const { return m_unit; }
    qreal size(qreal ratio) const { return m_unit * ratio; }
    QPointF point(qreal xRatio, qreal yRatio) const
    {
        return QPointF(m_bounds.left() + m_bounds.width() * xRatio,
                       m_bounds.top() + m_bounds.height() * yRatio);
    }
    QRectF rect(qreal xRatio, qreal yRatio,
                qreal widthRatio, qreal heightRatio) const
    {
        return QRectF(m_bounds.left() + m_bounds.width() * xRatio,
                      m_bounds.top() + m_bounds.height() * yRatio,
                      m_bounds.width() * widthRatio,
                      m_bounds.height() * heightRatio);
    }
    qreal stroke(const QPainter& painter, qreal ratio) const
    {
        const QTransform transform = painter.combinedTransform();
        const qreal deviceScale = qMax<qreal>(0.001,
            qMax(qAbs(transform.m11()), qAbs(transform.m22())));
        return qMax(1.0 / deviceScale, size(ratio));
    }

private:
    QRectF m_bounds;
    qreal m_unit;
};

// 根据字形度量定位基线，避免矩形文字绘制裁掉上下伸出的笔画。
inline QRectF drawSingleLine(QPainter& painter, const QRectF& area,
                             Qt::Alignment alignment, const QString& value)
{
    if (value.isEmpty() || area.isEmpty())
        return {};

    const QFont originalFont = painter.font();
    QFont fittedFont = originalFont;
    QFontMetricsF metrics(fittedFont);
    qreal textWidth = metrics.tightBoundingRect(value).width();
    if (textWidth > area.width() && area.width() > 0.0) {
        const qreal factor = area.width() / textWidth;
        if (fittedFont.pixelSize() > 0)
            fittedFont.setPixelSize(qMax(1, qFloor(fittedFont.pixelSize() * factor)));
        else
            fittedFont.setPointSizeF(qMax<qreal>(0.5,
                fittedFont.pointSizeF() * factor));
        painter.setFont(fittedFont);
        metrics = QFontMetricsF(fittedFont);
        textWidth = metrics.tightBoundingRect(value).width();
    }

    const QRectF glyphBounds = metrics.tightBoundingRect(value);
    qreal x = area.left();
    if (alignment.testFlag(Qt::AlignHCenter))
        x = area.center().x() - (glyphBounds.left() + glyphBounds.right()) * .5;
    else if (alignment.testFlag(Qt::AlignRight))
        x = area.right() - glyphBounds.right();
    else
        x -= glyphBounds.left();

    qreal baseline = area.top() + metrics.ascent();
    if (alignment.testFlag(Qt::AlignVCenter))
        baseline = area.center().y()
                 + (metrics.ascent() - metrics.descent()) * .5;
    else if (alignment.testFlag(Qt::AlignBottom))
        baseline = area.bottom() - metrics.descent();
    const bool previousTextAntialiasing = painter.testRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing,
        !(fittedFont.styleStrategy() & QFont::NoAntialias));
    if (!drawHighQualityText(painter, QPointF(x, baseline), value))
        painter.drawText(QPointF(x, baseline), value);
    painter.setRenderHint(QPainter::TextAntialiasing, previousTextAntialiasing);
    painter.setFont(originalFont);
    return glyphBounds.translated(x, baseline);
}

inline QRectF drawImageFitted(QPainter& painter, const QRectF& area,
                              const QImage& image)
{
    if (image.isNull() || area.isEmpty())
        return {};
    QSizeF fitted = image.size();
    fitted.scale(area.size(), Qt::KeepAspectRatio);
    const QRectF target(area.center().x() - fitted.width() * .5,
                        area.center().y() - fitted.height() * .5,
                        fitted.width(), fitted.height());
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image);
    painter.restore();
    return target;
}

inline void drawLine(QPainter& painter, const Metrics& metrics,
                     const QPointF& from, const QPointF& to,
                     const QColor& color, qreal widthRatio,
                     Qt::PenCapStyle cap = Qt::RoundCap)
{
    painter.setPen(QPen(color, metrics.stroke(painter, widthRatio),
                        Qt::SolidLine, cap));
    painter.drawLine(from, to);
}

// 用归一化数值绘制圆角区间条，可用于温度、电量和播放进度。
inline void drawRangeBar(QPainter& painter, const Metrics& metrics,
                         const QRectF& area, qreal fromRatio, qreal toRatio,
                         const QColor& trackColor, const QColor& rangeColor,
                         qreal widthRatio, qreal minimumRangeRatio = .025)
{
    if (area.isEmpty())
        return;

    qreal from = qBound<qreal>(0.0, qMin(fromRatio, toRatio), 1.0);
    qreal to = qBound<qreal>(0.0, qMax(fromRatio, toRatio), 1.0);
    const qreal minimum = qBound<qreal>(0.0, minimumRangeRatio, 1.0);
    if (to - from < minimum) {
        const qreal center = (from + to) * .5;
        from = qMax<qreal>(0.0, center - minimum * .5);
        to = qMin<qreal>(1.0, from + minimum);
        from = qMax<qreal>(0.0, to - minimum);
    }

    const qreal y = area.center().y();
    drawLine(painter, metrics, QPointF(area.left(), y),
             QPointF(area.right(), y), trackColor, widthRatio);
    drawLine(painter, metrics,
             QPointF(area.left() + area.width() * from, y),
             QPointF(area.left() + area.width() * to, y),
             rangeColor, widthRatio);
}

}
