#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QDateTime>
#include <QtMath>

using namespace CardMetrics;
using AppFonts::displayFont;

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
    const auto hand = [&painter, c](qreal angle, qreal length, qreal width, const QColor& color) {
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

void DesktopWidget::paintClockCard(QPainter& painter, const QRectF& card) const
{
    painter.save();
    const ResponsiveLayout::Metrics clockMetrics(card);
    const qreal inset = clockMetrics.size(kClockCardInsetRatio);
    drawClockFace(painter, card.adjusted(inset, inset, -inset, -inset), m_cardVariant,
                  (m_cardVariant == 3 || m_cardVariant == 4) ? QStringLiteral("北京") : QString(),
                  m_cardVariant == 4 ? 8 : 0);
    painter.restore();
}
