#include "widgets/countdowncard.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QFontMetrics>
#include <QLocale>
#include <QPainter>

namespace CountdownCard {
State state(const Event& event, const QDate& today)
{
    if (!today.isValid() || !event.date.isValid())
        return {};
    QDate occurrence = event.date;
    if (event.annual) {
        const auto anniversary = [&](int year) {
            // 二月二十九日的纪念日在平年按二月最后一天计算。
            const QDate month(year, event.date.month(), 1);
            return QDate(year, event.date.month(), qMin(event.date.day(), month.daysInMonth()));
        };
        const int year = qMax(today.year(), event.date.year());
        occurrence = anniversary(year);
        if (occurrence < today)
            occurrence = anniversary(year + 1);
    }
    const qint64 distance = today.daysTo(occurrence);
    return {occurrence, qAbs(distance), distance < 0};
}

void paint(QPainter& painter, const QRectF& bounds, const Event& event, const QDate& today)
{
    const State value = state(event, today);
    const ResponsiveLayout::Metrics m(bounds);
    const bool wide = bounds.width() > bounds.height() * 1.5;
    const QColor primary(250, 250, 252, 245);
    const QColor secondary(235, 237, 242, 160);
    const QColor accent(255, 114, 121);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const auto text = [&](const QRectF& area, const QString& label, qreal size,
                          const QColor& ink, QFont::Weight weight = QFont::Normal,
                          Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter,
                          bool elide = true) {
        QFont font(AppFonts::family());
        font.setPixelSize(qMax(1, qRound(m.size(size))));
        font.setWeight(weight);
        AppFonts::applySmoothing(font);
        painter.setFont(font);
        painter.setPen(ink);
        return ResponsiveLayout::drawSingleLine(painter, area, alignment, elide
            ? QFontMetrics(font).elidedText(label, Qt::ElideRight, qFloor(area.width())) : label);
    };
    const QRectF icon = m.rect(.08, .08, wide ? .047 : .095, .095);
    const ResponsiveLayout::Metrics c(icon);
    painter.setPen(QPen(accent, c.size(.09), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(c.rect(.07, .16, .86, .77), c.size(.13), c.size(.13));
    painter.drawLine(c.point(.07, .39), c.point(.93, .39));
    painter.drawLine(c.point(.30, .06), c.point(.30, .25));
    painter.drawLine(c.point(.70, .06), c.point(.70, .25));
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(c.rect(.27, .56, .14, .14));
    painter.drawEllipse(c.rect(.58, .56, .14, .14));
    text(m.rect(wide ? .14 : .21, .08, .42, .095),
         event.annual ? QStringLiteral("纪念日") : QStringLiteral("倒数日"), .057, accent, QFont::DemiBold);

    const QString phase = !value.occurrence.isValid() ? QStringLiteral("请选择日期")
        : value.days == 0 ? QStringLiteral("就是今天")
        : value.elapsed ? QStringLiteral("已经过去") : QStringLiteral("还有");
    const QString number = value.days == 0 ? QStringLiteral("0") : QString::number(value.days);
    const QString title = event.title.trimmed().isEmpty() ? QStringLiteral("值得期待的一天") : event.title;
    const QLocale chinese(QLocale::Chinese, QLocale::China);
    const QString date = value.occurrence.isValid()
        ? chinese.toString(value.occurrence, QStringLiteral("yyyy年M月d日 ddd")) : QString();
    if (wide) {
        text(m.rect(.08, .31, .50, .13), title, .095, primary, QFont::DemiBold);
        text(m.rect(.08, .49, .50, .08), date, .047, secondary);
        if (event.annual)
            text(m.rect(.08, .80, .46, .07), QStringLiteral("每年重复"), .043, secondary);
        text(m.rect(.64, .19, .28, .09), phase, .052, secondary, QFont::Medium, Qt::AlignCenter);
        text(m.rect(.61, .30, .34, .36), number, .31, primary, QFont::Light, Qt::AlignCenter, false);
        text(m.rect(.64, .74, .28, .08), QStringLiteral("天"), .055, secondary, QFont::Medium, Qt::AlignCenter);
    } else {
        text(m.rect(.08, .25, .84, .10), title, .075, primary, QFont::DemiBold);
        text(m.rect(.08, .39, .55, .07), phase, .046, secondary);
        const QRectF digits = text(m.rect(.08, .44, .68, .32), number, .30, primary, QFont::Light,
                                  Qt::AlignLeft | Qt::AlignVCenter, false);
        const qreal unitX = qMin(bounds.right() - m.size(.13), digits.right() + m.size(.035));
        text(QRectF(unitX, bounds.top() + m.size(.63), m.size(.10), m.size(.08)),
             QStringLiteral("天"), .054, secondary, QFont::Medium);
        text(m.rect(.08, .83, .84, .075), date, .043, secondary);
    }
    painter.restore();
}
}
