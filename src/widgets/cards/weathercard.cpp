#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QFontMetricsF>

using namespace CardMetrics;
using AppFonts::displayFont;

#include "widgets/weatherformat.h"
#include <QHash>
#include <limits>

using namespace WeatherFormat;

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

void DesktopWidget::paintWeatherCard(QPainter& painter, const QRectF& card) const
{
    painter.save();
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
    const auto elided = [](const QString& value, const QFont& font,
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
        const qreal iconSize = unit * (darkModeEnabled() ? .10 : .14);
        const qreal conditionWidth = iconSize * (darkModeEnabled() ? 1.0 : 1.35);
        const QRectF conditionColumn(content.right() - conditionWidth,
                                     content.top(), conditionWidth, h * .27);
        const QRectF summaryIcon(conditionColumn.center().x() - iconSize * .5,
                                 content.top() + unit * (darkModeEnabled() ? .020 : -.050),
                                 iconSize, iconSize);
        const QRectF paintedSummaryIcon = drawWeatherIcon(
            painter, summaryIcon, m_weatherNight, m_weatherCode);
        painter.setFont(weatherFont(kWeatherWideConditionFontRatio,
                              QFont::DemiBold));
        painter.setPen(QColor(232, 237, 245, 220));
        const qreal conditionTextTop = paintedSummaryIcon.bottom()
                                       + unit * kWeatherConditionGapRatio;
        const QRectF conditionText(
            summaryLeft, conditionTextTop,
            summaryWidth,
            qMax<qreal>(0.0, conditionColumn.bottom() - conditionTextTop));
        ResponsiveLayout::drawSingleLine(
            painter, conditionText, Qt::AlignRight | Qt::AlignVCenter,
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
                const qreal icon = unit * (darkModeEnabled() ? .125 : .14);
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
}
