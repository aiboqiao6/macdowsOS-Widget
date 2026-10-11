#include "widgets/musiccard.h"
#include "app/fontsettings.h"
#include "rendering/liquidglassstyle.h"
#include "rendering/responsivelayout.h"
#include "widgets/cardmetrics.h"
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace MusicCard {
namespace {
void note(QPainter& painter, const QRectF& bounds, const QColor& ink)
{
    const ResponsiveLayout::Metrics m(bounds);
    QPainterPath path;
    path.moveTo(m.point(.40, .25));
    path.lineTo(m.point(.78, .16));
    path.lineTo(m.point(.78, .70));
    path.cubicTo(m.point(.78, .86), m.point(.52, .90), m.point(.52, .76));
    path.cubicTo(m.point(.52, .66), m.point(.65, .62), m.point(.70, .65));
    path.lineTo(m.point(.70, .35));
    path.lineTo(m.point(.48, .40));
    path.lineTo(m.point(.48, .78));
    path.cubicTo(m.point(.48, .94), m.point(.22, .98), m.point(.22, .84));
    path.cubicTo(m.point(.22, .74), m.point(.35, .70), m.point(.40, .73));
    path.closeSubpath();
    painter.fillPath(path, ink);
}

QString timeLabel(qint64 milliseconds)
{
    const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}

Layout layout(const QRectF& bounds)
{
    const ResponsiveLayout::Metrics m(bounds);
    const bool wide = bounds.width() > bounds.height() * 1.5;
    Layout result;
    if (wide) {
        const qreal inset = m.size(.073);
        const qreal cover = bounds.height() - inset * 2;
        result.artwork = QRectF(bounds.topLeft() + QPointF(inset, inset), QSizeF(cover, cover));
        const qreal x = result.artwork.right() + m.size(.085);
        const qreal width = bounds.right() - inset - x;
        const auto area = [&](qreal y, qreal height) {
            return QRectF(x, bounds.top() + m.size(y), width, m.size(height));
        };
        result.branding = area(.068, .075);
        result.title = area(.245, .125);
        result.artist = area(.375, .09);
        result.progress = area(.535, .012);
        result.elapsed = QRectF(x, bounds.top() + m.size(.57), width / 2, m.size(.06));
        result.remaining = result.elapsed.translated(width / 2, 0);
        const qreal button = m.size(.182);
        result.toggle = QRectF(QPointF(x + width / 2 - button / 2, bounds.top() + m.size(.705)), QSizeF(button, button));
        result.previous = result.toggle.translated(-m.size(.29), 0);
        result.next = result.toggle.translated(m.size(.29), 0);
    } else {
        result.artwork = m.rect(.073, .073, .53, .53);
        result.branding = m.rect(.745, .073, .18, .18);
        result.title = m.rect(.073, .68, .66, .105);
        result.artist = m.rect(.073, .795, .66, .075);
        result.toggle = m.rect(.765, .745, .17, .17);
        // 紧凑卡片只保留播放按钮，空区域不会误触上一首与下一首。
    }
    return result;
}

void paint(QPainter& painter, const QRectF& bounds, const MediaSnapshot& media)
{
    const auto regions = layout(bounds);
    const ResponsiveLayout::Metrics m(bounds);
    const bool wide = !regions.progress.isEmpty();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal cardRadius = m.size(LiquidGlassStyle::kCardCornerRadius / CardMetrics::kGridBaseCell);
    // 封面与卡片采用同心的大圆角，扣除边距后保持内外轮廓协调。
    const qreal artworkRadius = qMax(m.size(.04), cardRadius - m.size(.073));
    QPainterPath cardClip;
    cardClip.addRoundedRect(bounds, cardRadius, cardRadius);
    painter.setClipPath(cardClip, Qt::IntersectClip);
    // 整面材质由宿主液态玻璃提供，专辑图像仅绘制在封面区域。

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 32));
    painter.drawRoundedRect(regions.artwork.translated(0, m.size(.012)), artworkRadius, artworkRadius);
    QPainterPath artworkClip;
    artworkClip.addRoundedRect(regions.artwork, artworkRadius, artworkRadius);
    painter.save();
    painter.setClipPath(artworkClip, Qt::IntersectClip);
    if (!media.artwork.isNull()) {
        const qreal side = qMin(media.artwork.width(), media.artwork.height());
        const QRectF source((media.artwork.width() - side) / 2,
                            (media.artwork.height() - side) / 2, side, side);
        painter.drawImage(regions.artwork, media.artwork, source);
    } else {
        QLinearGradient placeholder(regions.artwork.topLeft(), regions.artwork.bottomLeft());
        placeholder.setColorAt(0, QColor(255, 83, 116));
        placeholder.setColorAt(1, QColor(228, 39, 78));
        painter.fillRect(regions.artwork, placeholder);
        const qreal inset = regions.artwork.width() * .18;
        note(painter, regions.artwork.adjusted(inset, inset, -inset, -inset), Qt::white);
    }
    painter.restore();

    const auto text = [&](const QRectF& area, const QString& value, qreal ratio,
                          const QColor& color, QFont::Weight weight = QFont::Normal,
                          Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter) {
        const QFont font = AppFonts::displayFont(qMax(1, qRound(m.size(ratio))), weight);
        painter.setFont(font);
        painter.setPen(color);
        ResponsiveLayout::drawSingleLine(painter, area, alignment,
            QFontMetrics(font).elidedText(value, Qt::ElideRight, qFloor(area.width())));
    };
    if (wide) {
        const QRectF symbol(regions.branding.topLeft(), QSizeF(m.size(.065), m.size(.065)));
        note(painter, symbol, QColor(255, 255, 255, 235));
        text(regions.branding.adjusted(m.size(.077), 0, 0, 0), QStringLiteral("Music"), .046,
             QColor(255, 255, 255, 235), QFont::DemiBold);
    } else {
        note(painter, regions.branding, QColor(255, 255, 255, 235));
    }
    const QString title = media.available
        ? (media.title.isEmpty() ? QStringLiteral("未知曲目") : media.title)
        : QStringLiteral("未在播放");
    const QString artist = media.available
        ? (media.artist.isEmpty() ? QStringLiteral("未知艺术家") : media.artist)
        : (!media.error.isEmpty() ? QStringLiteral("暂时无法读取音乐") : QStringLiteral("打开音乐播放器"));
    text(regions.title, title, .068, Qt::white, QFont::DemiBold);
    text(regions.artist, artist, .043, QColor(255, 255, 255, 166));

    if (wide) {
        const qint64 position = qBound<qint64>(0, media.positionMs, qMax<qint64>(0, media.durationMs));
        const qreal progress = media.durationMs > 0 ? position / qreal(media.durationMs) : 0;
        const qreal y = regions.progress.center().y();
        painter.setPen(QPen(QColor(255, 255, 255, 48), m.size(.009), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(regions.progress.left(), y), QPointF(regions.progress.right(), y));
        if (progress > 0) {
            painter.setPen(QPen(QColor(255, 255, 255, 210), m.size(.009), Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(regions.progress.left(), y), QPointF(regions.progress.left() + regions.progress.width() * progress, y));
        }
        const bool hasTime = media.available && media.durationMs > 0;
        text(regions.elapsed, hasTime ? timeLabel(position) : QStringLiteral("–:––"), .027, QColor(255, 255, 255, 145));
        text(regions.remaining, hasTime ? QLatin1Char('-') + timeLabel(media.durationMs - position) : QStringLiteral("–:––"),
             .027, QColor(255, 255, 255, 145), QFont::Normal, Qt::AlignRight | Qt::AlignVCenter);
    }
    const auto control = [&](const QRectF& area, bool enabled, int direction) {
        if (area.isEmpty())
            return;
        const ResponsiveLayout::Metrics b(area);
        painter.setPen(Qt::NoPen);
        QColor ink(255, 255, 255, enabled ? 255 : 80);
        if (direction == 0) {
            painter.setBrush(QColor(255, 255, 255, enabled ? 245 : 45));
            painter.drawEllipse(area);
            ink = enabled ? QColor(30, 34, 40) : QColor(255, 255, 255, 100);
        }
        painter.setBrush(ink);
        if (direction == 0 && media.playing) {
            painter.drawRoundedRect(b.rect(.345, .29, .115, .42), b.size(.025), b.size(.025));
            painter.drawRoundedRect(b.rect(.54, .29, .115, .42), b.size(.025), b.size(.025));
        } else if (direction == 0) {
            QPainterPath play;
            play.moveTo(b.point(.40, .28));
            play.lineTo(b.point(.73, .50));
            play.lineTo(b.point(.40, .72));
            play.closeSubpath();
            painter.drawPath(play);
        } else {
            // 双三角快退、快进接近系统媒体符号，命中区域仍保持足够大。
            for (qreal offset : {.20, .48}) {
                QPolygonF triangle;
                triangle << b.point(direction < 0 ? offset + .27 : offset, .31)
                         << b.point(direction < 0 ? offset + .27 : offset, .69)
                         << b.point(direction < 0 ? offset : offset + .27, .50);
                painter.drawPolygon(triangle);
            }
        }
    };
    control(regions.previous, media.available && media.canPrevious, -1);
    control(regions.toggle, media.available && media.canToggle, 0);
    control(regions.next, media.available && media.canNext, 1);
    painter.restore();
}

MediaSnapshot previewSnapshot()
{
    static const QImage artwork = [] {
        QImage image(512, 512, QImage::Format_RGB32);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0, 0, 512, 512);
        sky.setColorAt(0, QColor(226, 123, 87));
        sky.setColorAt(.52, QColor(120, 54, 70));
        sky.setColorAt(1, QColor(33, 33, 55));
        painter.fillRect(image.rect(), sky);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 208, 148));
        painter.drawEllipse(QRectF(215, 94, 178, 178));
        QPainterPath hills;
        hills.moveTo(-30, 375);
        hills.cubicTo(150, 160, 210, 400, 540, 238);
        hills.lineTo(540, 540); hills.lineTo(-30, 540); hills.closeSubpath();
        painter.fillPath(hills, QColor(59, 49, 69));
        QPainterPath foreground;
        foreground.moveTo(-30, 420);
        foreground.cubicTo(200, 280, 310, 510, 540, 370);
        foreground.lineTo(540, 540); foreground.lineTo(-30, 540); foreground.closeSubpath();
        painter.fillPath(foreground, QColor(30, 34, 49));
        return image;
    }();
    MediaSnapshot media;
    media.available = media.playing = true;
    media.canToggle = media.canPrevious = media.canNext = true;
    media.title = QStringLiteral("日落之后");
    media.artist = QStringLiteral("轻音乐精选");
    media.positionMs = 74000;
    media.durationMs = 238000;
    media.artwork = artwork;
    return media;
}
}
