#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "widgets/weatherformat.h"
#include "widgets/musiccard.h"
#include "rendering/responsivelayout.h"
#include <QDate>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QTime>
#include <QFile>
#include <limits>

using namespace CardMetrics;
using namespace WeatherFormat;
namespace {
QFont displayFont(int pointSize, QFont::Weight weight = QFont::Normal)
{
    QFont font;
    font.setFamily(DesktopWidget::pingFangFontFamily());
    font.setPointSize(pointSize);
    font.setWeight(weight);
    DesktopWidget::applyFontSmoothing(font);
    return font;
}
}

QRectF DesktopWidget::drawWeatherIcon(QPainter& painter, const QRectF& rect, bool night,
                                      int weatherCode) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 使用随程序打包的白色天气素材，保留原始抗锯齿透明通道。
    QString iconName = weatherCode >= 0 ? QString::number(weatherCode)
                                         : QStringLiteral("01");
    if (weatherCode >= 0 && weatherCode < 10)
        iconName = iconName.rightJustified(2, QLatin1Char('0'));
    if (night && (iconName == QStringLiteral("00")
                  || iconName == QStringLiteral("01")
                  || iconName == QStringLiteral("03")
                  || iconName == QStringLiteral("13")))
        iconName += QLatin1Char('n');

    static QHash<QString, QImage> iconCache;
    auto it = iconCache.constFind(iconName);
    if (it == iconCache.cend()) {
        QImage source(QStringLiteral(":/weather/%1.png").arg(iconName));
        if (source.isNull())
            source.load(QStringLiteral(":/weather/01.png"));
        source = source.convertToFormat(QImage::Format_ARGB32);

        // 裁掉天气素材的透明外边距，确保图标真实大小与视觉中心正确。
        int left = source.width();
        int top = source.height();
        int right = -1;
        int bottom = -1;
        for (int y = 0; y < source.height(); ++y) {
            const QRgb* scan = reinterpret_cast<const QRgb*>(source.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                if (qAlpha(scan[x]) == 0)
                    continue;
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }
        if (right >= left && bottom >= top)
            source = source.copy(QRect(left, top, right - left + 1, bottom - top + 1));
        it = iconCache.constFind(iconCache.insert(iconName, source).key());
    }

    const QRectF paintedBounds = ResponsiveLayout::drawImageFitted(painter, rect, it.value());
    painter.restore();
    return paintedBounds;
}

void DesktopWidget::drawAnalogClock(QPainter& painter, const QRectF& rect, int hourOffset) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);
    const QPointF c = rect.center();
    const qreal r = qMin(rect.width(), rect.height()) * kClockDialRadiusRatio;
    painter.setPen(QPen(QColor(247, 250, 255, 72), metrics.stroke(painter, .004)));
    painter.setBrush(QColor(255, 255, 255, 16));
    painter.drawEllipse(c, r, r);
    for (int i = 0; i < 60; ++i) {
        const qreal a = (i / 60.0) * 2.0 * M_PI - M_PI / 2.0;
        const qreal outer = r - metrics.size(.012);
        const qreal inner = r - metrics.size((i % 5 == 0) ? .052 : .028);
        painter.setPen(QPen(QColor(245, 249, 255, (i % 5 == 0) ? 210 : 100),
                      metrics.stroke(painter, (i % 5 == 0) ? .008 : .004),
                      Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(c + QPointF(std::cos(a), std::sin(a)) * inner,
                   c + QPointF(std::cos(a), std::sin(a)) * outer);
    }
    painter.setFont(displayFont(qMax(13, qRound(r * .23)), QFont::DemiBold));
    painter.setPen(QColor(238, 243, 253, 218));
    for (int hour = 1; hour <= 12; ++hour) {
        const qreal a = (hour / 12.0) * 2.0 * M_PI - M_PI / 2.0;
        const QPointF pos = c + QPointF(std::cos(a), std::sin(a))
                          * (r - metrics.size(.108));
        const QString label = QString::number(hour);
        const qreal labelBox = metrics.size(.104);
        ResponsiveLayout::drawSingleLine(painter, QRectF(pos.x() - labelBox * .5,
                                                    pos.y() - labelBox * .5,
                                                    labelBox, labelBox),
                       Qt::AlignCenter, label);
    }
    const QTime time = QTime::currentTime().addSecs(hourOffset * 3600);
    const auto hand = [&painter, c, r](qreal angle, qreal length, qreal width, const QColor& color) {
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(c, c + QPointF(std::cos(angle), std::sin(angle)) * length);
    };
    const qreal sec = (time.second() + time.msec() / 1000.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal min = (time.minute() + time.second() / 60.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal hour = ((time.hour() % 12) + time.minute() / 60.0) / 12.0 * 2.0 * M_PI - M_PI / 2.0;
    hand(hour, r * .52, metrics.stroke(painter, .020), QColor(250, 252, 255, 230));
    hand(min, r * .73, metrics.stroke(painter, .013), QColor(250, 252, 255, 220));
    hand(sec, r * .80, metrics.stroke(painter, .006), QColor(255, 174, 172, 230));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 195, 194, 240));
    painter.drawEllipse(c, metrics.size(.016), metrics.size(.016));
    painter.restore();
}

void DesktopWidget::drawDigitalClock(QPainter& painter, const QRectF& rect,
                                     const QString& city, int hourOffset,
                                     bool transparentFace) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);
    const qreal faceInset = metrics.size(kClockFaceInsetRatio);
    const QRectF face = rect.adjusted(faceInset, faceInset,
                                      -faceInset, -faceInset);
    if (!transparentFace) {
        painter.setPen(QPen(QColor(255, 255, 255, 150), metrics.stroke(painter, .004)));
        painter.setBrush(QColor(248, 249, 251, 238));
        painter.drawRoundedRect(face, qMin(face.width(), face.height()) * .16,
                          qMin(face.width(), face.height()) * .16);
    }
    const QDateTime value = QDateTime::currentDateTime().addSecs(hourOffset * 3600);
    const bool wideLayout = face.width() >= face.height() * 1.35;
    painter.setPen(transparentFace ? QColor(246, 249, 255, 238)
                             : QColor(14, 18, 24, 240));
    painter.setFont(displayFont(qMax(22, qRound(face.height()
                                          * (wideLayout ? .38 : .23))),
                          QFont::DemiBold));
    const QRectF timeArea = wideLayout
        ? QRectF(face.left() + face.width() * .04,
                 face.top() + face.height() * .06,
                 face.width() * .92, face.height() * .66)
        : face.adjusted(face.width() * .08, face.height() * .20,
                        -face.width() * .08, -face.height() * .22);
    ResponsiveLayout::drawSingleLine(painter, timeArea, Qt::AlignCenter,
                                     value.toString(QStringLiteral("HH:mm")));
    painter.setPen(transparentFace ? QColor(220, 229, 244, 220)
                             : QColor(111, 119, 133, 210));
    painter.setFont(displayFont(qMax(8, qRound(face.height()
                                         * (wideLayout ? .085 : .075))),
                          QFont::Normal));
    const QRectF locationArea = wideLayout
        ? QRectF(face.left(), face.top() + face.height() * .72,
                 face.width(), face.height() * .18)
        : face.adjusted(0, face.height() * .73, 0, -face.height() * .08);
    ResponsiveLayout::drawSingleLine(
        painter, locationArea, Qt::AlignCenter,
        city.isEmpty() ? QStringLiteral("LOCAL") : city.toUpper());
    // 横向数字时钟使用完整文字区域，方形布局才使用环形刻度。
    if (wideLayout) {
        painter.restore();
        return;
    }
    painter.setPen(QPen(transparentFace ? QColor(235, 242, 255, 150)
                                 : QColor(74, 81, 92, 130), metrics.stroke(painter, .003),
                  Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 48; ++i) {
        const qreal angle = (i / 48.0) * 2.0 * M_PI - M_PI / 2.0;
        const qreal outer = qMin(face.width(), face.height()) * .43;
        const qreal inner = outer - metrics.size((i % 4 == 0) ? .020 : .010);
        const QPointF c = face.center();
        painter.drawLine(c + QPointF(std::cos(angle), std::sin(angle)) * inner,
                   c + QPointF(std::cos(angle), std::sin(angle)) * outer);
    }
    painter.restore();
}

void DesktopWidget::drawClockFace(QPainter& painter, const QRectF& rect, int style,
                                  const QString& city, int hourOffset) const
{
    const ResponsiveLayout::Metrics metrics(rect);
    if (style == 2 || style == 8) {
        drawDigitalClock(painter, rect, city, hourOffset, true);
        return;
    }
    if (style == 5 || style == 6 || style == 7) {
        painter.save();
        const int count = style == 5 ? 4 : (style == 6 ? 3 : 4);
        const int cols = style == 5 ? 2 : (style == 6 ? 3 : 4);
        const qreal gap = rect.width() * .05;
        const qreal cellW = (rect.width() - gap * (cols - 1)) / cols;
        const qreal cellH = style == 5 ? (rect.height() - gap) / 2.0 : rect.height();
        const QStringList cities = { QStringLiteral("东京"), QStringLiteral("北京"),
                                     QStringLiteral("巴黎"), QStringLiteral("纽约") };
        const int offsets[] = { 9, 8, 1, -5 };
        for (int i = 0; i < count; ++i) {
            const QRectF cell(rect.left() + (i % cols) * (cellW + gap),
                              rect.top() + (i / cols) * (cellH + gap), cellW, cellH);
            painter.setPen(QPen(QColor(255, 255, 255, 72), metrics.stroke(painter, .003)));
            painter.setBrush(QColor(32, 37, 48, style == 7 ? 198 : 182));
            painter.drawRoundedRect(cell, qMin(cellW, cellH) * .16, qMin(cellW, cellH) * .16);
            drawAnalogClock(painter, cell.adjusted(cellW * .08, cellH * .08,
                                             -cellW * .08, -cellH * .08), offsets[i]);
            painter.setPen(QColor(225, 232, 244, 185));
            painter.setFont(displayFont(qMax(8, qRound(cellH * .07)), QFont::Normal));
            ResponsiveLayout::drawSingleLine(painter, QRectF(cell.left(), cell.bottom() - cellH * .19,
                                     cell.width(), cellH * .12),
                           Qt::AlignCenter, cities.at(i));
        }
        painter.restore();
        return;
    }

    painter.save();
    const qreal faceInset = metrics.size(kClockFaceInsetRatio);
    const QRectF face = rect.adjusted(faceInset, faceInset,
                                      -faceInset, -faceInset);
    if (style == 1 || style == 4) {
        painter.setPen(QPen(QColor(255, 255, 255, 50), metrics.stroke(painter, .003)));
        painter.setBrush(QColor(26, 30, 38, 215));
        painter.drawRoundedRect(face, qMin(face.width(), face.height()) * .16,
                          qMin(face.width(), face.height()) * .16);
    }
    QRectF dialArea = face;
    QRectF cityArea;
    if (!city.isEmpty()) {
        const qreal labelHeight = rect.height() * kClockCityLabelHeightRatio;
        const qreal labelGap = rect.height() * kClockCityLabelGapRatio;
        cityArea = QRectF(face.left(), face.bottom() - labelHeight,
                          face.width(), labelHeight);
        dialArea.setBottom(cityArea.top() - labelGap);
    }
    const qreal dialInset = metrics.size(kClockDialInsetRatio);
    const QRectF dial = style == 1 || style == 4
        ? dialArea.adjusted(dialInset, dialInset, -dialInset, -dialInset)
        : dialArea;
    drawAnalogClock(painter, dial, hourOffset);
    if (!city.isEmpty()) {
        painter.setPen(style == 1 || style == 4 ? QColor(235, 239, 247, 205)
                                         : QColor(89, 98, 112, 205));
        painter.setFont(displayFont(qMax(8, qRound(rect.height() * .065)), QFont::DemiBold));
        ResponsiveLayout::drawSingleLine(painter, cityArea, Qt::AlignCenter, city);
    }
    painter.restore();
}

void DesktopWidget::drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);

    const qreal radius = rect.height() * 0.21;
    const qreal edge = metrics.stroke(painter, .012);
    QRectF body = rect.adjusted(edge, edge, -rect.height() * 0.105, -edge);
    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, radius, radius);

    QLinearGradient shell(0, body.top(), 0, body.bottom());
    shell.setColorAt(0.0, QColor(255, 255, 255, 220));
    shell.setColorAt(0.45, QColor(221, 227, 236, 165));
    shell.setColorAt(1.0, QColor(157, 166, 180, 160));
    painter.fillPath(bodyPath, shell);

    const qreal inset = metrics.size(.09);
    QRectF levelRect = body.adjusted(inset, inset, -inset, -inset);
    levelRect.setWidth(levelRect.width() * level / 100.0);
    if (level > 0) {
        QPainterPath levelPath;
        levelPath.addRoundedRect(levelRect, radius * 0.68, radius * 0.68);
        QLinearGradient fill(levelRect.topLeft(), levelRect.bottomRight());
        if (level <= 20) {
            fill.setColorAt(0.0, QColor(255, 142, 91));
            fill.setColorAt(1.0, QColor(235, 58, 82));
        } else if (charging) {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        } else {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        }
        painter.fillPath(levelPath, fill);
    }

    QRectF terminal(body.right() + metrics.size(.02),
                    body.center().y() - rect.height() * .16,
                    rect.height() * .105, rect.height() * .32);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(208, 215, 225, 190));
    painter.drawRoundedRect(terminal, terminal.width() * .5, terminal.width() * .5);

    painter.setPen(QPen(QColor(255, 255, 255, 160),
                        metrics.stroke(painter, .012)));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(bodyPath);

    if (charging) {
        QPainterPath bolt;
        const qreal cx = body.center().x();
        const qreal cy = body.center().y();
        bolt.moveTo(cx + rect.width() * .015, cy - rect.height() * .24);
        bolt.lineTo(cx - rect.width() * .095, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .005, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .065, cy + rect.height() * .25);
        bolt.lineTo(cx + rect.width() * .12, cy - rect.height() * .055);
        bolt.lineTo(cx + rect.width() * .025, cy - rect.height() * .055);
        bolt.closeSubpath();
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.setPen(Qt::NoPen);
        painter.drawPath(bolt);
    }
    painter.restore();
}

void DesktopWidget::drawDeviceRing(QPainter& painter, const QPointF& center, qreal radius,
                                   int level, int iconKind, bool connected,
                                   bool showCaption, qreal strokeMultiplier) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0);
    const ResponsiveLayout::Metrics metrics(ring);
    const qreal stroke = metrics.stroke(painter, .0525)
                       * qMax<qreal>(0.1, strokeMultiplier);

    QPen track(QColor(238, 242, 250, connected ? 38 : 24), stroke,
               Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(track);
    painter.drawEllipse(ring);

    const int clamped = qBound(0, level, 100);
    if (connected && level >= 0 && clamped > 0) {
        const qreal span = -360.0 * clamped / 100.0;
        QPen progress(QColor(255, 255, 255, 242), stroke,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(progress);
        constexpr qreal startAngle = 90.0;
        painter.drawArc(ring, qRound(startAngle * 16.0), qRound(span * 16.0));

    }

    // 使用中性线条设备图标，未连接位置保留空环。
    if (iconKind == 0) {
        const QRectF screen(center.x() - radius * .34, center.y() - radius * .19,
                            radius * .68, radius * .44);
        painter.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(screen, radius * .06, radius * .06);
        painter.drawLine(QPointF(center.x() - radius * .45, center.y() + radius * .29),
                   QPointF(center.x() + radius * .45, center.y() + radius * .29));
        painter.drawLine(QPointF(center.x() - radius * .28, center.y() + radius * .15),
                   QPointF(center.x() + radius * .28, center.y() + radius * .15));
    } else if (iconKind == 1) {
        const QRectF mouse(center.x() - radius * .19, center.y() - radius * .40,
                           radius * .38, radius * .74);
        painter.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(226, 232, 244, connected ? 138 : 45));
        painter.drawRoundedRect(mouse, radius * .18, radius * .18);
        painter.drawLine(QPointF(center.x(), mouse.top() + radius * .08),
                   QPointF(center.x(), mouse.top() + radius * .29));
    } else if (iconKind == 2 && connected) {
        const QRectF keyboard(center.x() - radius * .39, center.y() - radius * .21,
                              radius * .78, radius * .42);
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(226, 232, 244, 34));
        painter.drawRoundedRect(keyboard, radius * .07, radius * .07);
        painter.drawLine(keyboard.bottomLeft() + QPointF(radius * .12, -radius * .10),
                   keyboard.bottomRight() + QPointF(-radius * .12, -radius * .10));
    } else if (iconKind == 3 && connected) {
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .20,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(center.x() - radius * .31, center.y() - radius * .31,
                         radius * .62, radius * .62), 0, 180 * 16);
        painter.drawLine(QPointF(center.x() - radius * .31, center.y()),
                   QPointF(center.x() - radius * .31, center.y() + radius * .28));
        painter.drawLine(QPointF(center.x() + radius * .31, center.y()),
                   QPointF(center.x() + radius * .31, center.y() + radius * .28));
    } else if (iconKind == 4 && connected) {
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18));
        painter.setBrush(QColor(226, 232, 244, 34));
        painter.drawRoundedRect(QRectF(center.x() - radius * .24, center.y() - radius * .34,
                                 radius * .48, radius * .68), radius * .10, radius * .10);
    }

    // 空槽不显示说明，仅真实连接设备显示文字。
    if (showCaption && (connected || iconKind == 0)) {
        painter.setFont(displayFont(qMax(1, qRound(radius * .44)), QFont::Normal));
        painter.setPen(QColor(235, 240, 250, connected ? 182 : 66));
        ResponsiveLayout::drawSingleLine(painter, QRectF(center.x() - radius * .72,
                                 center.y() + radius * 1.55,
                                 radius * 1.44, radius * .85),
                       Qt::AlignHCenter | Qt::AlignTop,
                       !connected ? QStringLiteral("—")
                                  : (level >= 0
                                         ? QString::number(clamped) + QChar('%')
                                         : QStringLiteral("已连接")));
    }
    painter.restore();
}

void DesktopWidget::paintOverlay(QPainter& painter)
{
    const qreal scale = qMax<qreal>(0.1, m_widgetScale);
    painter.save();
    painter.scale(scale, scale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, globalFontSmoothing() > 0);

    // 内容按 100% 参考坐标绘制，只缩放一次；玻璃覆盖完整窗口，避免额外内缩产生外圈。
    const QSizeF logicalSize(width() / scale, height() / scale);
    const QRectF card(QPointF(0.0, 0.0), logicalSize);
    const qreal radius = 34.0;

    // 内容高光与着色器共享圆角裁剪，轮廓之外保持透明。
    QPainterPath cardClip;
    cardClip.addRoundedRect(card, radius, radius);
    painter.setClipPath(cardClip, Qt::IntersectClip);

    paintGlassSurfaceFrame(painter, card, radius);

    if (m_cardKind == CardKind::Music) {
        MusicCard::paint(painter, card, SystemMediaSession::instance()->snapshot());
        painter.restore();
        return;
    }

    // 独立窗口只绘制自身内容，玻璃基类负责模糊、裁剪和壁纸采样。
    if (m_cardKind == CardKind::Battery && (m_cardVariant == 0 || m_cardVariant == 1)) {
        const bool hasMouse = !m_connectedDevices.isEmpty();
        const ConnectedDevice mouse = hasMouse ? m_connectedDevices.first() : ConnectedDevice{};
        // 仅绘制系统实际报告的设备和电量，空槽保留安静的玻璃圆环。
            const qreal ringRadius = qMin(card.width(), card.height()) * .190;
            if (m_cardVariant == 0) {
            // 四个圆环向卡片边缘展开，保持等距留白。
            const qreal left = card.left() + card.width() * .265;
            const qreal right = card.left() + card.width() * .735;
            const qreal top = card.top() + card.height() * .265;
            const qreal bottom = card.top() + card.height() * .735;
            drawDeviceRing(painter, QPointF(left, top), ringRadius,
                           m_hasBattery ? m_batteryLevel : -1, 0, m_hasBattery,
                           false, 1.30);
            drawDeviceRing(painter, QPointF(right, top), ringRadius,
                           hasMouse ? mouse.level : -1,
                           hasMouse ? mouse.iconKind : -1, hasMouse,
                           false, 1.30);
            drawDeviceRing(painter, QPointF(left, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
            drawDeviceRing(painter, QPointF(right, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
        } else {
            const qreal sideInset = card.width() * .145;
            const qreal firstX = card.left() + sideInset;
            const qreal spacing = (card.width() - sideInset * 2.0) / 3.0;
            const qreal ringY = card.top() + card.height() * .42;
            drawDeviceRing(painter, QPointF(firstX, ringY), ringRadius,
                           m_hasBattery ? m_batteryLevel : -1, 0, m_hasBattery);
            for (int slot = 0; slot < 3; ++slot) {
                const bool connected = slot < m_connectedDevices.size();
                const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                          : ConnectedDevice{};
                drawDeviceRing(painter, QPointF(firstX + spacing * (slot + 1), ringY),
                               ringRadius, device.level,
                               connected ? device.iconKind : -1, connected);
            }
        }
        painter.restore();
        return;
    }

    if (m_cardKind == CardKind::Battery && m_cardVariant == 2) {
        // 设备列表采用紧凑行高和适度边距，避免文字过大导致拥挤。
        const ResponsiveLayout::Metrics cardMetrics(card);
        const qreal padding = cardMetrics.size(.036);
        const QRectF content = card.adjusted(padding, padding, -padding, -padding);
        const auto row = [&painter](const QRectF& area, const QString& name,
                              int level, int iconKind) {
            const ResponsiveLayout::Metrics rowMetrics(area);
            const qreal s = area.height();
            painter.setPen(QPen(QColor(244, 248, 255, 210),
                          rowMetrics.stroke(painter, .015), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            const QPointF iconCenter(area.left() + s * .365, area.center().y());
            if (iconKind == 0) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .163,
                                         iconCenter.y() - s * .106,
                                         s * .327, s * .212),
                                  s * .019, s * .019);
                painter.drawLine(QPointF(iconCenter.x() - s * .231,
                                   iconCenter.y() + s * .154),
                           QPointF(iconCenter.x() + s * .231,
                                   iconCenter.y() + s * .154));
            } else if (iconKind == 1) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .096,
                                         iconCenter.y() - s * .163,
                                         s * .192, s * .327),
                                  s * .077, s * .077);
                painter.drawLine(QPointF(iconCenter.x(), iconCenter.y() - s * .106),
                           QPointF(iconCenter.x(), iconCenter.y() - s * .019));
            } else if (iconKind == 2) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .192,
                                         iconCenter.y() - s * .115,
                                         s * .385, s * .231),
                                  s * .029, s * .029);
                painter.drawLine(QPointF(iconCenter.x() - s * .135,
                                   iconCenter.y() + s * .048),
                           QPointF(iconCenter.x() + s * .135,
                                   iconCenter.y() + s * .048));
            } else if (iconKind == 3) {
                painter.drawArc(QRectF(iconCenter.x() - s * .163,
                                 iconCenter.y() - s * .163,
                                 s * .327, s * .327), 0, 180 * 16);
                painter.drawLine(QPointF(iconCenter.x() - s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() - s * .163,
                                   iconCenter.y() + s * .135));
                painter.drawLine(QPointF(iconCenter.x() + s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() + s * .163,
                                   iconCenter.y() + s * .135));
            } else {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .115,
                                         iconCenter.y() - s * .163,
                                         s * .231, s * .327),
                                  s * .038, s * .038);
            }
            painter.setPen(QColor(246, 249, 255, 235));
            painter.setFont(displayFont(qMax(1, qRound(s * .192)), QFont::DemiBold));
            const int nameWidth = qMax(qRound(s * .77),
                                       qRound(area.width() - s * 2.31));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.left() + s * .942, area.top(),
                          nameWidth, area.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(name, Qt::ElideRight, nameWidth));
            painter.setFont(displayFont(qMax(1, qRound(s * .173)), QFont::Normal));
            painter.setPen(QColor(246, 249, 255, 235));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.right() - s * 1.471, area.top(),
                          s * .50, area.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                level >= 0 ? QString::number(qBound(0, level, 100)) + QChar('%')
                           : QStringLiteral("—"));
            painter.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(painter, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            const QRectF batteryRect(area.right() - s * .769,
                                     area.center().y() - s * .077,
                                     s * .346, s * .154);
            painter.drawRoundedRect(batteryRect, s * .038, s * .038);
            if (level >= 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 235));
                const qreal inset = s * .038;
                const qreal fillWidth = (batteryRect.width() - inset * 2.0)
                    * qBound(0, level, 100) / 100.0;
                if (fillWidth > 0.0)
                    painter.drawRoundedRect(QRectF(batteryRect.left() + inset,
                                             batteryRect.top() + s * .029,
                                             fillWidth,
                                             batteryRect.height() - s * .058),
                                      s * .019, s * .019);
            }
            painter.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(painter, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(QPointF(area.right() - s * .385,
                               area.center().y() - s * .029),
                       QPointF(area.right() - s * .327,
                               area.center().y() - s * .029));
        };
        // 六行等高，只有真实设备填充内容，剩余行保持空白。
        const qreal rowHeight = content.height() / kBatteryListRows;
        const QRectF firstRow(content.left(), content.top(), content.width(), rowHeight);
        const QRectF secondRow(content.left(), content.top() + rowHeight,
                               content.width(), rowHeight);
        row(firstRow, m_hostDeviceName,
            m_hasBattery ? m_batteryLevel : -1, 0);
        for (int slot = 0; slot < qMin(kBatteryListRows - 1,
                                       int(m_connectedDevices.size())); ++slot) {
            const ConnectedDevice& device = m_connectedDevices.at(slot);
            const QRectF deviceRow(content.left(), secondRow.top() + rowHeight * slot,
                                   content.width(), rowHeight);
            row(deviceRow,
                device.name.isEmpty() ? QStringLiteral("已连接设备") : device.name,
                device.level, device.iconKind);
        }
        painter.setPen(QPen(QColor(255, 255, 255, 34),
                      cardMetrics.stroke(painter, .0015)));
        for (int i = 1; i < kBatteryListRows; ++i) {
            const qreal y = content.top() + rowHeight * i;
            painter.drawLine(QPointF(content.left() + rowHeight * .942, y),
                       QPointF(content.right(), y));
        }
        painter.restore();
        return;
    }

    if (m_cardKind == CardKind::Weather) {
        // 天气卡片的边距、图标与字号统一按短边参考单位计算。
        const qreal shortEdge = qMin(card.width(), card.height());
        // 2×2 天气卡片沿用单网格的字号与图标尺寸，只增加信息容量。
        const qreal unit = qMax<qreal>(1.0,
            m_cardVariant == 2 ? shortEdge * .5 : shortEdge);
        const qreal xPad = unit * .11;
        const qreal yPad = unit * .095;
        const QRectF content = card.adjusted(xPad, yPad, -xPad, -yPad);
        const qreal w = content.width();
        const qreal h = content.height();
        const ResponsiveLayout::Metrics weatherMetrics(card);
        const auto weatherFont = [unit](qreal ratio, QFont::Weight weight) {
            return displayFont(qMax(8, qRound(unit * ratio)), weight);
        };
        const auto elided = [&painter](const QString& value, const QFont& font,
                                 qreal width) {
            return QFontMetrics(font).elidedText(value, Qt::ElideRight,
                                                 qMax(1, qRound(width)));
        };
        const QString temp = weatherTemperatureLabel(m_weatherTemperature);
        const QString description = m_weatherDescription.isEmpty()
            ? QStringLiteral("获取天气中…") : m_weatherDescription;
        const auto drawLocation = [&painter, unit, &elided](const QRectF& area,
                                                       const QString& value) {
            const QString label = elided(value, painter.font(), area.width() - unit * .08);
            ResponsiveLayout::drawSingleLine(painter, area, Qt::AlignLeft | Qt::AlignVCenter, label);
            const qreal textWidth = QFontMetricsF(painter.font()).horizontalAdvance(label);
            const qreal size = unit * .052;
            const QPointF anchor(area.left() + textWidth + unit * .020,
                                 area.center().y());
            QPainterPath marker;
            marker.moveTo(anchor.x(), anchor.y() - size * .10);
            marker.lineTo(anchor.x() + size, anchor.y() - size * .42);
            marker.lineTo(anchor.x() + size * .64, anchor.y() + size * .52);
            marker.lineTo(anchor.x() + size * .44, anchor.y() + size * .14);
            marker.closeSubpath();
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(224, 231, 241, 210));
            painter.drawPath(marker);
            painter.restore();
        };
        const auto drawHighLow = [&painter, &weatherFont, unit](
                                                          const QRectF& area,
                                                          const QString& high,
                                                          const QString& low,
                                                          qreal valueFontRatio) {
            const qreal groupWidth = area.width() * .50;
            const auto group = [&painter, &weatherFont, unit, valueFontRatio](const QRectF& box,
                                                        const QString& label,
                                                        const QString& value) {
                const qreal labelWidth = box.width() * .25;
                painter.setFont(weatherFont(.040, QFont::DemiBold));
                painter.setPen(QColor(205, 213, 226, 205));
                const qreal half = box.height() * .50;
                ResponsiveLayout::drawSingleLine(painter, QRectF(box.left(), box.top(), labelWidth, half),
                                Qt::AlignCenter, label.left(1));
                ResponsiveLayout::drawSingleLine(painter, QRectF(box.left(), box.top() + half,
                                       labelWidth, half),
                                Qt::AlignCenter, label.mid(1, 1));
                painter.setFont(weatherFont(valueFontRatio, QFont::Normal));
                painter.setPen(QColor(235, 240, 247, 225));
                ResponsiveLayout::drawSingleLine(painter, QRectF(box.left() + labelWidth + unit * .006,
                                       box.top(), box.width() - labelWidth,
                                       box.height()),
                                Qt::AlignLeft | Qt::AlignVCenter,
                                weatherTemperatureLabel(value));
            };
            group(QRectF(area.left(), area.top(), groupWidth, area.height()),
                  QStringLiteral("最高"), high);
            group(QRectF(area.left() + groupWidth, area.top(),
                         groupWidth, area.height()),
                  QStringLiteral("最低"), low);
        };

        if (m_cardVariant == 0) {
            painter.setFont(weatherFont(.075, QFont::DemiBold));
            painter.setPen(QColor(240, 244, 250, 228));
            drawLocation(QRectF(content.left(), content.top(), w, h * .105),
                         m_weatherLocation);

            // 紧凑天气卡片依次排列地点、温度、天气状态和高低温。
            painter.setFont(weatherFont(.22, QFont::Normal));
            painter.setPen(QColor(248, 250, 253, 242));
            const QRectF temperatureArea(content.left(), content.top() + h * .17,
                                         w, h * .27);
            const QRectF temperatureBounds = ResponsiveLayout::drawSingleLine(
                painter, temperatureArea, Qt::AlignLeft | Qt::AlignVCenter, temp);
            const QRectF highLowArea(content.left(), content.top() + h * .755,
                                     w, h * .17);
            const qreal iconSize = unit * .115;
            const qreal flowGap = unit * .018;
            const qreal descriptionHeight = h * .10;
            const qreal preferredIconTop = content.top() + h * .44;
            const qreal latestIconTop = highLowArea.top() - flowGap
                                      - descriptionHeight - flowGap - iconSize;
            const qreal iconTop = qMin(latestIconTop,
                qMax(preferredIconTop, temperatureBounds.bottom() + flowGap));
            const QRectF iconArea(content.left(), iconTop, iconSize, iconSize);
            drawWeatherIcon(painter, iconArea,
                            m_weatherNight, m_weatherCode);
            painter.setFont(weatherFont(kWeatherCompactConditionFontRatio,
                                  QFont::DemiBold));
            painter.setPen(QColor(235, 239, 246, 220));
            // 短天气描述居中于图标列，长描述从同一前沿展开。
            const qreal measuredDescriptionWidth =
                QFontMetricsF(painter.font()).tightBoundingRect(description).width();
            const qreal descriptionWidth = qMin(
                w * .72, qMax(iconSize, measuredDescriptionWidth));
            const QRectF descriptionArea(content.left(), iconArea.bottom() + flowGap,
                                         descriptionWidth, descriptionHeight);
            ResponsiveLayout::drawSingleLine(
                painter, descriptionArea, Qt::AlignCenter,
                elided(description, painter.font(), descriptionArea.width()));
            drawHighLow(highLowArea, m_weatherHigh, m_weatherLow, .115);
            painter.restore();
            return;
        }

        if (m_cardVariant == 1) {
            painter.setFont(weatherFont(.068, QFont::DemiBold));
            painter.setPen(QColor(240, 244, 250, 228));
            drawLocation(QRectF(content.left(), content.top(), w * .56, h * .105),
                         m_weatherLocation);
            painter.setFont(weatherFont(.205, QFont::Normal));
            painter.setPen(QColor(248, 250, 253, 242));
            ResponsiveLayout::drawSingleLine(painter, QRectF(content.left(), content.top() + h * .17,
                                   w * .43, h * .27),
                            Qt::AlignLeft | Qt::AlignVCenter, temp);
            const qreal summaryWidth = w * .37;
            const qreal summaryLeft = content.right() - summaryWidth;
            const qreal iconSize = unit * .14;
            const qreal conditionWidth = iconSize * 1.35;
            const QRectF conditionColumn(content.right() - conditionWidth,
                                         content.top(), conditionWidth, h * .27);
            const QRectF summaryIcon(conditionColumn.center().x() - iconSize * .5,
                                     content.top() - unit * .050,
                                     iconSize, iconSize);
            const QRectF paintedSummaryIcon = drawWeatherIcon(
                painter, summaryIcon, m_weatherNight, m_weatherCode);
            painter.setFont(weatherFont(kWeatherWideConditionFontRatio,
                                  QFont::DemiBold));
            painter.setPen(QColor(232, 237, 245, 220));
            const qreal conditionTextTop = paintedSummaryIcon.bottom()
                                           + unit * kWeatherConditionGapRatio;
            const QRectF conditionText(
                conditionColumn.left(), conditionTextTop,
                conditionColumn.width(),
                qMax<qreal>(0.0, conditionColumn.bottom() - conditionTextTop));
            ResponsiveLayout::drawSingleLine(
                painter, conditionText, Qt::AlignCenter,
                elided(description, painter.font(), conditionText.width()));
            drawHighLow(QRectF(summaryLeft, content.top() + h * .28,
                               summaryWidth, h * .15),
                        m_weatherHigh, m_weatherLow, .115);
            const qreal stripTop = content.top() + h * .535;
            const qreal stripHeight = h * .40;
            const int hourCount = qMin(kWeatherHourlySlots, m_weatherHours.size());
            if (hourCount > 0) {
                // 部分网络回复仍保留六列布局，后续数据到达时不改变已有位置。
                const qreal cellW = w / kWeatherHourlySlots;
                for (int i = 0; i < hourCount; ++i) {
                    const WeatherHour& hour = m_weatherHours.at(i);
                    const QRectF cell(content.left() + cellW * i, stripTop,
                                      cellW, stripHeight);
                    painter.setFont(weatherFont(.060, QFont::Normal));
                    painter.setPen(QColor(193, 204, 222, 190));
                    ResponsiveLayout::drawSingleLine(painter, QRectF(cell.left(), cell.top(), cell.width(),
                                           stripHeight * .18),
                                    Qt::AlignCenter, hour.time);
                    const qreal icon = unit * .14;
                    drawWeatherIcon(painter, QRectF(cell.center().x() - icon * .5,
                                              cell.top() + stripHeight * .24,
                                              icon, icon * .84),
                                    hour.night, hour.code);
                    painter.setFont(weatherFont(.060, QFont::DemiBold));
                    painter.setPen(QColor(235, 240, 247, 218));
                    ResponsiveLayout::drawSingleLine(painter, QRectF(cell.left(),
                                           cell.top() + stripHeight * .88,
                                           cell.width(), stripHeight * .18),
                                    Qt::AlignCenter,
                                    weatherTemperatureLabel(hour.temperature));
                }
            }
            painter.restore();
            return;
        }

        painter.setFont(weatherFont(kWeatherLargeLocationFontRatio, QFont::DemiBold));
        painter.setPen(QColor(241, 245, 251, 232));
        drawLocation(QRectF(content.left(), content.top(), w * .56, unit * .11),
                     m_weatherLocation);
        painter.setFont(weatherFont(kWeatherLargeTemperatureFontRatio, QFont::Normal));
        painter.setPen(QColor(249, 251, 254, 242));
        ResponsiveLayout::drawSingleLine(painter, QRectF(content.left(), content.top() + unit * .155,
                               w * .45, unit * .25),
                        Qt::AlignLeft | Qt::AlignVCenter, temp);
        // 右侧用紧凑的图标与状态摘要显示当前天气，高低温位于其下方。
        const qreal summaryWidth = w * .35;
        const qreal summaryLeft = content.right() - summaryWidth;
        const qreal iconSize = unit * .14;
        const qreal conditionWidth = iconSize * 1.35;
        const QRectF conditionColumn(content.right() - conditionWidth,
                                     content.top(), conditionWidth, unit * .22);
        const QRectF summaryIcon(conditionColumn.center().x() - iconSize * .5,
                                 content.top() + unit * .025,
                                 iconSize, iconSize * .84);
        const QRectF paintedSummaryIcon = drawWeatherIcon(
            painter, summaryIcon, m_weatherNight, m_weatherCode);
        painter.setFont(weatherFont(kWeatherLargeConditionFontRatio, QFont::Normal));
        painter.setPen(QColor(202, 211, 226, 195));
        const qreal conditionTextTop = paintedSummaryIcon.bottom()
                                       + unit * kWeatherConditionGapRatio;
        const QRectF conditionText(conditionColumn.left(), conditionTextTop,
                                   conditionColumn.width(),
                                   qMax<qreal>(0.0,
                                       conditionColumn.bottom()
                                       - conditionTextTop));
        ResponsiveLayout::drawSingleLine(
            painter, conditionText, Qt::AlignCenter,
            elided(description, painter.font(), conditionText.width()));
        drawHighLow(QRectF(summaryLeft, content.top() + unit * .230,
                           summaryWidth, unit * .15),
                    m_weatherHigh, m_weatherLow,
                    kWeatherLargeRangeFontRatio);
        // 当前天气、六小时预报和五日列表分别使用固定比例分区。
        const qreal cardHeight = card.height();
        const qreal summaryDividerY = card.top()
                                      + cardHeight * kWeatherSummaryDividerRatio;
        const qreal hourlyTop = card.top()
                                + cardHeight * kWeatherHourlyTopRatio;
        const int hourCount = qMin(kWeatherHourlySlots, m_weatherHours.size());
        if (hourCount > 0) {
            const qreal cellW = w / kWeatherHourlySlots;
            for (int i = 0; i < hourCount; ++i) {
                const WeatherHour& hour = m_weatherHours.at(i);
                const QRectF cell(content.left() + cellW * i, hourlyTop,
                                  cellW, cardHeight * .20);
                painter.setFont(weatherFont(kWeatherLargeHourlyFontRatio, QFont::Normal));
                painter.setPen(QColor(196, 206, 222, 195));
                ResponsiveLayout::drawSingleLine(painter, QRectF(cell.left(), cell.top(), cell.width(),
                                       cardHeight * .055),
                                Qt::AlignCenter, hour.time);
                const qreal icon = unit * .14;
                drawWeatherIcon(painter, QRectF(cell.center().x() - icon * .5,
                                          hourlyTop + cardHeight
                                                      * kWeatherHourlyIconOffsetRatio,
                                          icon, icon * .82),
                                hour.night, hour.code);
                painter.setFont(weatherFont(kWeatherLargeHourlyFontRatio, QFont::DemiBold));
                painter.setPen(QColor(241, 245, 251, 225));
                ResponsiveLayout::drawSingleLine(painter, QRectF(cell.left(),
                                       hourlyTop + cardHeight
                                                   * kWeatherHourlyTemperatureOffsetRatio,
                                       cell.width(), cardHeight * .055),
                                Qt::AlignCenter,
                                weatherTemperatureLabel(hour.temperature));
            }
        }
        // 小时文字完成后重绘分隔线，避免后续文字批次影响抗锯齿边缘。
        ResponsiveLayout::drawLine(
            painter, weatherMetrics, QPointF(content.left(), summaryDividerY),
            QPointF(content.right(), summaryDividerY),
            QColor(255, 255, 255, 105), .003);
        const qreal dividerY = card.top()
                               + cardHeight * kWeatherDailyDividerRatio;
        ResponsiveLayout::drawLine(
            painter, weatherMetrics, QPointF(content.left(), dividerY),
            QPointF(content.right(), dividerY),
            QColor(255, 255, 255, 105), .003);
        const int dayStart = !m_weatherDays.isEmpty()
                             && m_weatherDays.first().label == QStringLiteral("今天")
                           ? 1 : 0;
        const int dayCount = qMin(kWeatherDailySlots,
                                  qMax(0, m_weatherDays.size() - dayStart));
        const qreal rowTop = card.top() + cardHeight * .56;
        // 部分预报仍保留五行布局，避免单日数据撑满整个区域。
        const qreal rowH = qMax<qreal>(
            0.0, (content.bottom() - rowTop) / kWeatherDailySlots);

        // 所有日期共用温度数值范围，区间条的位置可直接比较。
        const auto numericTemperature = [](QString value, qreal* result) {
            value.remove(QChar(0x2103));
            value.remove(QChar(0x00B0));
            bool ok = false;
            const qreal number = value.trimmed().toDouble(&ok);
            if (ok && result)
                *result = number;
            return ok;
        };
        qreal scaleMinimum = std::numeric_limits<qreal>::max();
        qreal scaleMaximum = std::numeric_limits<qreal>::lowest();
        bool hasTemperatureScale = false;
        for (int i = 0; i < dayCount; ++i) {
            const WeatherDay& day = m_weatherDays.at(i + dayStart);
            qreal value = 0.0;
            if (numericTemperature(day.low, &value)) {
                scaleMinimum = qMin(scaleMinimum, value);
                scaleMaximum = qMax(scaleMaximum, value);
                hasTemperatureScale = true;
            }
            if (numericTemperature(day.high, &value)) {
                scaleMinimum = qMin(scaleMinimum, value);
                scaleMaximum = qMax(scaleMaximum, value);
                hasTemperatureScale = true;
            }
        }
        const qreal temperatureSpan = scaleMaximum - scaleMinimum;
        for (int i = 0; i < dayCount; ++i) {
            const WeatherDay& day = m_weatherDays.at(i + dayStart);
            const QRectF row(content.left(), rowTop + rowH * i, content.width(), rowH);
            painter.setFont(weatherFont(kWeatherLargeDailyLabelFontRatio,
                                  QFont::DemiBold));
            painter.setPen(QColor(226, 232, 242, 210));
            ResponsiveLayout::drawSingleLine(painter, QRectF(row.left(), row.top(), w * .12, row.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            normalizedWeekdayLabel(day.label));
            const qreal icon = unit * .13;
            drawWeatherIcon(painter, QRectF(row.left() + w * .13,
                                      row.center().y() - icon * .5,
                                      icon, icon * .88),
                            day.night, day.code);
            painter.setFont(weatherFont(kWeatherLargeDailyTemperatureFontRatio,
                                  QFont::DemiBold));
            painter.setPen(QColor(219, 227, 239, 204));
            ResponsiveLayout::drawSingleLine(painter, QRectF(row.left() + w * .30, row.top(),
                                   w * .12, row.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            weatherTemperatureLabel(day.low));
            const QRectF rangeArea(row.left() + w * .47,
                                   row.center().y() - row.height() * .12,
                                   row.width() - w * .59,
                                   row.height() * .24);
            qreal low = 0.0;
            qreal high = 0.0;
            const bool hasLow = numericTemperature(day.low, &low);
            const bool hasHigh = numericTemperature(day.high, &high);
            if (hasTemperatureScale && hasLow && hasHigh) {
                const qreal lowRatio = qFuzzyIsNull(temperatureSpan)
                    ? .5 : (low - scaleMinimum) / temperatureSpan;
                const qreal highRatio = qFuzzyIsNull(temperatureSpan)
                    ? .5 : (high - scaleMinimum) / temperatureSpan;
                ResponsiveLayout::drawRangeBar(
                    painter, weatherMetrics, rangeArea, lowRatio, highRatio,
                    QColor(255, 255, 255, 28),
                    QColor(238, 242, 248, 185), .010, .035);
            } else {
                ResponsiveLayout::drawLine(
                    painter, weatherMetrics,
                    QPointF(rangeArea.left(), rangeArea.center().y()),
                    QPointF(rangeArea.right(), rangeArea.center().y()),
                    QColor(255, 255, 255, 28), .010);
            }
            painter.setPen(QColor(219, 227, 239, 204));
            ResponsiveLayout::drawSingleLine(painter, QRectF(row.right() - w * .11, row.top(),
                                   w * .11, row.height()),
                            Qt::AlignRight | Qt::AlignVCenter,
                            weatherTemperatureLabel(day.high));
        }
        painter.restore();
        return;
    }

    if (m_cardKind == CardKind::Clock) {
        const ResponsiveLayout::Metrics clockMetrics(card);
        const qreal inset = clockMetrics.size(kClockCardInsetRatio);
        drawClockFace(painter, card.adjusted(inset, inset, -inset, -inset), m_cardVariant,
                      (m_cardVariant == 3 || m_cardVariant == 4) ? QStringLiteral("北京") : QString(),
                      m_cardVariant == 4 ? 8 : 0);
        painter.restore();
        return;
    }

    if (m_cardKind == CardKind::Dictionary) {
        const ResponsiveLayout::Metrics dictionaryMetrics(card);
        if (m_cardVariant == 0 || m_cardVariant == 2) {
            const QRectF content = card.adjusted(28, 26, -28, -24);
            painter.setFont(displayFont(m_cardVariant == 0 ? 26 : 30, QFont::DemiBold));
            painter.setPen(QColor(247, 250, 255, 238));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(content.left(), content.top(), content.width(), 42),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…")
                                           : m_dictionaryWord);
            painter.setFont(displayFont(13, QFont::Normal));
            painter.setPen(QColor(191, 203, 224, 205));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(content.left(), content.top() + 52, content.width(), 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_dictionaryPhonetic.isEmpty() ? QStringLiteral("每日单词")
                                               : m_dictionaryPhonetic);
            painter.setFont(displayFont(11, QFont::Normal));
            painter.setPen(QColor(170, 184, 211, 185));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(content.left(), content.bottom() - 30,
                          content.width(), 20),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_cardVariant == 0 ? QStringLiteral("点击刷新")
                               : QStringLiteral("在线词典"));
            painter.restore();
            return;
        }
        const QRectF content = card.adjusted(32, 26, -30, -24);
        painter.setFont(displayFont(34, QFont::DemiBold));
        painter.setPen(QColor(244, 247, 254, 232));
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top(), content.width(), 42),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…") : m_dictionaryWord);
        painter.setFont(displayFont(18, QFont::Normal));
        painter.setPen(QColor(218, 226, 242, 220));
        const QString phonetic = m_dictionaryPhonetic.isEmpty()
                                     ? QStringLiteral("词典数据") : m_dictionaryPhonetic;
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top() + 46, content.width(), 28),
            Qt::AlignLeft | Qt::AlignVCenter, phonetic);
        painter.setFont(displayFont(16, QFont::Normal));
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top() + 82, content.width(), 28),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryPartOfSpeech.isEmpty() ? QStringLiteral("在线词典")
                                               : m_dictionaryPartOfSpeech);
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top() + 118, content.width(), 42),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(painter.font()).elidedText(m_dictionaryDefinition,
                                              Qt::ElideRight, int(content.width())));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(198, 210, 238, 105));
        const qreal barWidth = qMin<qreal>(content.width() - 110, 560.0);
        for (int i = 0; i < 20; ++i)
            painter.drawRoundedRect(QRectF(content.left() + i * (barWidth / 20.0),
                                     content.top() + 164, barWidth / 20.0 - 4, 20), 4, 4);
        painter.setPen(QPen(QColor(215, 226, 250, 180),
                      dictionaryMetrics.stroke(painter, .003)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(content.left(), card.bottom() - 52, 82, 32), 6, 6);
        painter.setFont(displayFont(14, QFont::Normal));
        ResponsiveLayout::drawSingleLine(painter, QRectF(content.left(), card.bottom() - 52,
                                             82, 32),
                                   Qt::AlignCenter, QStringLiteral("随机单词"));
        painter.restore();
        return;
    }

    painter.restore();
}
