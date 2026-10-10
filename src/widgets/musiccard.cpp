#include "widgets/musiccard.h"
#include "widgets/desktopwidget.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>

namespace MusicCard {
Layout layout(const QRectF& bounds)
{
    const ResponsiveLayout::Metrics metrics(bounds);
    const bool wide = bounds.width() > bounds.height() * 1.5;
    Layout result;
    result.artwork = wide ? metrics.rect(.045, .16, .32, .64) : metrics.rect(.07, .07, .34, .34);
    result.title = wide ? metrics.rect(.41, .17, .54, .13) : metrics.rect(.46, .10, .47, .12);
    result.artist = wide ? metrics.rect(.41, .32, .54, .09) : metrics.rect(.46, .24, .47, .09);
    result.status = wide ? metrics.rect(.41, .44, .54, .07) : metrics.rect(.07, .46, .86, .08);
    result.progress = wide ? metrics.rect(.41, .57, .54, .04) : metrics.rect(.07, .59, .86, .04);
    const qreal centerX = wide ? .68 : .5;
    const qreal y = wide ? .64 : .71;
    const qreal buttonWidth = metrics.size(.16) / bounds.width();
    const qreal gap = metrics.size(.24) / bounds.width();
    result.toggle = metrics.rect(centerX - buttonWidth / 2, y, buttonWidth, .16);
    result.previous = result.toggle.translated(-bounds.width() * gap, 0);
    result.next = result.toggle.translated(bounds.width() * gap, 0);
    return result;
}

void paint(QPainter& painter, const QRectF& bounds, const MediaSnapshot& media)
{
    const auto regions = layout(bounds);
    const ResponsiveLayout::Metrics metrics(bounds);
    painter.save();
    painter.setPen(Qt::NoPen);
    QLinearGradient tint(bounds.topLeft(), bounds.bottomRight());
    tint.setColorAt(0, QColor(137, 73, 190, 65));
    tint.setColorAt(1, QColor(25, 74, 106, 30));
    painter.fillRect(bounds, tint);
    QPainterPath artworkClip;
    artworkClip.addRoundedRect(regions.artwork, metrics.size(.04), metrics.size(.04));
    painter.save();
    painter.setClipPath(artworkClip, Qt::IntersectClip);
    painter.fillRect(regions.artwork, QColor(66, 43, 92, 210));
    if (!media.artwork.isNull()) {
        QSizeF artworkSize = media.artwork.size();
        artworkSize.scale(regions.artwork.size(), Qt::KeepAspectRatioByExpanding);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(regions.artwork.center() - QPointF(artworkSize.width()/2,
            artworkSize.height()/2), artworkSize), media.artwork);
    } else {
        const auto cover = ResponsiveLayout::Metrics(regions.artwork);
        painter.setPen(QPen(QColor(244, 214, 255), cover.size(.045), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(cover.point(.44, .27), cover.point(.44, .69));
        painter.drawLine(cover.point(.44, .27), cover.point(.69, .20));
        painter.drawLine(cover.point(.69, .20), cover.point(.69, .61));
        painter.setBrush(QColor(244, 214, 255));
        painter.drawEllipse(cover.rect(.27, .61, .18, .14));
        painter.drawEllipse(cover.rect(.52, .53, .18, .14));
    }
    painter.restore();
    const auto text = [&](const QRectF& area, const QString& value, qreal ratio,
                          const QColor& color, QFont::Weight weight = QFont::Normal) {
        QFont font(DesktopWidget::pingFangFontFamily());
        font.setPixelSize(qMax(1, qRound(metrics.size(ratio))));
        font.setWeight(weight);
        DesktopWidget::applyFontSmoothing(font);
        painter.setFont(font);
        painter.setPen(color);
        ResponsiveLayout::drawSingleLine(painter, area, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(font).elidedText(value, Qt::ElideRight, qFloor(area.width())));
    };
    text(regions.title, media.available ? (media.title.isEmpty() ? QStringLiteral("未知曲目") : media.title)
        : QStringLiteral("此刻的旋律"), .065, QColor(255, 248, 255), QFont::DemiBold);
    text(regions.artist, media.available ? (media.artist.isEmpty() ? QStringLiteral("未知艺术家") : media.artist)
        : QStringLiteral("等待系统播放器"), .042, QColor(233, 217, 244));
    const QString status = !media.error.isEmpty() ? media.error : !media.available
        ? QStringLiteral("打开支持系统媒体控件的播放器") : !media.playbackLabel.isEmpty()
        ? media.playbackLabel : media.playing ? QStringLiteral("正在播放") : QStringLiteral("已暂停");
    text(regions.status, status, .038, QColor(212, 198, 225));
    painter.setPen(QPen(QColor(255,255,255,48), metrics.size(.012), Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(regions.progress.topLeft(), regions.progress.topRight());
    if (media.durationMs > 0) {
        const qreal progress = qBound<qreal>(0, media.positionMs / qreal(media.durationMs), 1);
        painter.setPen(QPen(QColor(236, 183, 255), metrics.size(.012), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(regions.progress.topLeft(), regions.progress.topLeft() + QPointF(regions.progress.width() * progress, 0));
        const auto time = [](qint64 milliseconds) {
            const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
            return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
        };
        text(regions.progress.translated(0, metrics.size(.025)), time(media.positionMs)
            + QStringLiteral(" / ") + time(media.durationMs), .030, QColor(220, 205, 230));
    }
    const auto control = [&](const QRectF& area, bool enabled, int direction) {
        const QColor ink(255, 246, 255, enabled ? 245 : 70);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255,255,255, direction == 0 ? 33 : 13));
        painter.drawEllipse(area);
        const ResponsiveLayout::Metrics button(area);
        painter.setBrush(ink);
        if (direction == 0 && media.playing) {
            painter.drawRoundedRect(button.rect(.34,.28,.10,.44), 1, 1);
            painter.drawRoundedRect(button.rect(.56,.28,.10,.44), 1, 1);
        } else {
            QPolygonF triangle;
            triangle << button.point(direction < 0 ? .62 : .38, .28)
                     << button.point(direction < 0 ? .62 : .38, .72)
                     << button.point(direction < 0 ? .30 : .72, .50);
            painter.drawPolygon(triangle);
            if (direction != 0)
                painter.drawRect(button.rect(direction < 0 ? .26 : .72, .28, .055, .44));
        }
    };
    control(regions.previous, media.available && media.canPrevious, -1);
    control(regions.toggle, media.available && media.canToggle, 0);
    control(regions.next, media.available && media.canNext, 1);
    painter.restore();
}
}
