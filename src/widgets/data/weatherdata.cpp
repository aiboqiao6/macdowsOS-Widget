#include "widgets/desktopwidget.h"
#include "network/cardrequest.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDate>
#include <QTime>

#include "widgets/weatherformat.h"
#include "ui/weathercitydialog.h"
#include "app/appsettings.h"
#include <QRegularExpression>

using namespace WeatherFormat;

namespace {
QJsonObject jsonObjectAfterMarker(const QByteArray& payload, const QByteArray& marker)
{
    const int markerIndex = payload.indexOf(marker);
    const int objectStart = markerIndex < 0 ? -1 : payload.indexOf('{', markerIndex);
    if (objectStart < 0)
        return {};

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int i = objectStart; i < payload.size(); ++i) {
        const char ch = payload.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}' && --depth == 0) {
            const QJsonDocument document = QJsonDocument::fromJson(
                payload.mid(objectStart, i - objectStart + 1));
            return document.isObject() ? document.object() : QJsonObject{};
        }
    }
    return {};
}

QString weatherValue(const QJsonValue& value)
{
    QString result;
    if (value.isString())
        result = value.toString().trimmed();
    else if (value.isDouble())
        result = QString::number(qRound(value.toDouble()));
    result.remove(QChar(0x2103));
    result.remove(QChar(0x00B0));
    return result.trimmed();
}

int numericWeatherCode(const QString& value)
{
    QString digits;
    for (const QChar ch : value) {
        if (ch.isDigit())
            digits.append(ch);
    }
    bool ok = false;
    const int code = digits.toInt(&ok);
    return ok ? code : -1;
}

QString chineseWeekdayLabel(const QDate& date)
{
    static const QStringList labels = {
        QStringLiteral("周一"), QStringLiteral("周二"), QStringLiteral("周三"),
        QStringLiteral("周四"), QStringLiteral("周五"), QStringLiteral("周六"),
        QStringLiteral("周日")
    };
    const int index = qBound(1, date.dayOfWeek(), 7) - 1;
    return labels.at(index);
}
}

void DesktopWidget::refreshWeather()
{
    if (!m_weatherNetwork)
        return;

    ++m_weatherReplySerial;
    if (m_weatherReply)
        m_weatherReply->abort();
    const QString cityId = m_weatherCityId.isEmpty()
        ? QStringLiteral("101010100") : m_weatherCityId;
    QUrl url(QStringLiteral("https://d1.weather.com.cn/weather_index/%1.html").arg(cityId));
    QNetworkRequest request(url);
    CardRequest::configureWeather(request);
    // 刷新天气期间保留最后完整快照，避免清空内容造成闪烁。
    if (m_weatherTemperature.isEmpty()) {
        m_weatherDescription = QStringLiteral("获取天气中…");
        update();
    }
    QNetworkReply* reply = CardRequest::get(*m_weatherNetwork, request);
    m_weatherReply = reply;
    reply->setProperty("weatherSerial", m_weatherReplySerial);
}

void DesktopWidget::handleWeatherReply(QNetworkReply* reply)
{
    if (!reply)
        return;
    // 搜索回复由对话框独立处理，不能当作天气数据解析。
    if (reply->property("weatherSearch").toBool())
        return;
    const int serial = reply->property("weatherSerial").toInt();
    if (serial != m_weatherReplySerial) {
        reply->deleteLater();
        return;
    }

    if (reply->property("weatherDaily").toBool()) {
        const QString html = QString::fromUtf8(reply->readAll());
        const QRegularExpression liExpression(
            QStringLiteral(R"(<li\b[^>]*class\s*=\s*["']sky[^"']*["'][^>]*>(.*?)</li>)"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression codeExpression(
            QStringLiteral(R"(<big\b[^>]*class\s*=\s*["'][^"']*\bd(\d{1,2})\b[^"']*["'])"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression temperatureExpression(
            QStringLiteral(R"(<p\b[^>]*class\s*=\s*["'][^"']*\btem\b[^"']*["'][^>]*>.*?<span[^>]*>\s*(-?\d+)\s*</span>\s*/\s*<i[^>]*>\s*(-?\d+))"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
        QVector<WeatherDay> days;
        QRegularExpressionMatchIterator iterator = liExpression.globalMatch(html);
        int index = 0;
        while (iterator.hasNext() && days.size() < 5) {
            const QString card = iterator.next().captured(1);
            // 跳过今日，从明天开始显示五个完整预报日。
            if (index++ == 0)
                continue;
            const QRegularExpressionMatch codeMatch = codeExpression.match(card);
            const QRegularExpressionMatch temperatureMatch =
                temperatureExpression.match(card);
            if (!temperatureMatch.hasMatch())
                continue;
            WeatherDay item;
            item.label = chineseWeekdayLabel(QDate::currentDate().addDays(days.size() + 1));
            item.high = temperatureMatch.captured(1).trimmed();
            item.low = temperatureMatch.captured(2).trimmed();
            item.code = codeMatch.hasMatch() ? codeMatch.captured(1).toInt() : -1;
            item.night = false;
            days.append(item);
        }
        if (days.size() >= 4)
            m_weatherDays = days;
        reply->deleteLater();
        update();
        return;
    }

    if (reply->property("weatherHourly").toBool()) {
        const QByteArray payload = reply->readAll();
        const QJsonObject hourlyRoot = jsonObjectAfterMarker(payload, "var hour3data");
        QVector<WeatherHour> hours;
        const QJsonArray hourlyValues = hourlyRoot.value(QStringLiteral("1d")).toArray();
        for (const QJsonValue& value : hourlyValues) {
            const QStringList fields = value.toString().split(QLatin1Char(','));
            if (fields.size() < 4)
                continue;
            WeatherHour item;
            item.time = fields.at(0).trimmed();
            // 服务返回的时间含日期前缀，卡片只保留紧凑的小时部分。
            const int hourMarker = item.time.lastIndexOf(QStringLiteral("日"));
            if (hourMarker >= 0)
                item.time = item.time.mid(hourMarker + 1).trimmed();
            item.code = numericWeatherCode(fields.at(1));
            item.temperature = weatherValue(fields.at(3));
            item.night = fields.at(1).trimmed().startsWith(QLatin1Char('n'));
            hours.append(item);
            if (hours.size() == 6)
                break;
        }
        if (!hours.isEmpty())
            m_weatherHours = hours;
        reply->deleteLater();
        update();
        return;
    }

    const QByteArray payload = reply->readAll();
    const QJsonObject current = jsonObjectAfterMarker(payload, "var dataSK");
    const QJsonObject forecastRoot = jsonObjectAfterMarker(payload, "var fc");
    const QJsonObject cityRoot = jsonObjectAfterMarker(payload, "var cityDZ");
    const bool ok = reply->error() == QNetworkReply::NoError
        && !current.isEmpty() && !forecastRoot.isEmpty();
    if (ok) {
        const QJsonObject info = cityRoot.value(QStringLiteral("weatherinfo")).toObject();
        const QString apiLocation = info.value(QStringLiteral("city")).toString().trimmed();
        // 保留搜索对话框选定的精确区县名称，仅默认城市允许采用服务返回的规范名称。
        if (m_weatherLocation.isEmpty() || m_weatherCityId == QStringLiteral("101010100"))
            m_weatherLocation = apiLocation;
        if (m_weatherLocation.isEmpty())
            m_weatherLocation = current.value(QStringLiteral("cityname")).toString().trimmed();
        m_weatherTemperature = weatherValue(current.value(QStringLiteral("temp")));
        m_weatherDescription = current.value(QStringLiteral("weather")).toString().trimmed();
        if (m_weatherDescription.isEmpty())
            m_weatherDescription = info.value(QStringLiteral("weather")).toString().trimmed();
        m_weatherCode = numericWeatherCode(current.value(QStringLiteral("weathercode")).toString());
        if (m_weatherCode < 0)
            m_weatherCode = numericWeatherCode(info.value(QStringLiteral("weathercode")).toString());
        const QString nightCode = info.value(QStringLiteral("weathercoden")).toString();
        m_weatherNight = nightCode.startsWith(QLatin1Char('n'))
                      || QTime::currentTime().hour() < 6
                      || QTime::currentTime().hour() >= 18;

        QVector<WeatherDay> parsedDays;
        const QJsonArray days = forecastRoot.value(QStringLiteral("f")).toArray();
        for (const QJsonValue& value : days) {
            const QJsonObject day = value.toObject();
            WeatherDay item;
            item.label = normalizedWeekdayLabel(
                day.value(QStringLiteral("fj")).toString());
            if (item.label.isEmpty())
                item.label = day.value(QStringLiteral("fi")).toString().trimmed();
            item.high = weatherValue(day.value(QStringLiteral("fc")));
            item.low = weatherValue(day.value(QStringLiteral("fd")));
            item.code = numericWeatherCode(day.value(QStringLiteral("fa")).toString());
            item.night = day.value(QStringLiteral("fb")).toString().trimmed()
                             .startsWith(QLatin1Char('n'));
            parsedDays.append(item);
        }
        if (!parsedDays.isEmpty()) {
            m_weatherHigh = parsedDays.first().high;
            m_weatherLow = parsedDays.first().low;
            // 一次替换完整列表，避免绘制时看到部分清空的状态。
            m_weatherDays = std::move(parsedDays);
        }

        QNetworkRequest hourlyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather1d/%1.shtml")
                .arg(m_weatherCityId)));
        CardRequest::configureWeather(hourlyRequest);
        QNetworkReply* hourlyReply = CardRequest::get(*m_weatherNetwork, hourlyRequest);
        m_weatherReply = hourlyReply;
        hourlyReply->setProperty("weatherSerial", m_weatherReplySerial);
        hourlyReply->setProperty("weatherHourly", true);
        // 首页预报天数不足时补充七日页面，以便从明天开始显示五行。
        QNetworkRequest dailyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather/%1.shtml")
                .arg(m_weatherCityId)));
        CardRequest::configureWeather(dailyRequest);
        QNetworkReply* dailyReply = CardRequest::get(*m_weatherNetwork, dailyRequest);
        dailyReply->setProperty("weatherSerial", m_weatherReplySerial);
        dailyReply->setProperty("weatherDaily", true);

    } else {
        m_weatherDescription = m_weatherTemperature.isEmpty()
            ? QStringLiteral("天气暂不可用") : QStringLiteral("更新失败 · 显示上次天气");
    }
    reply->deleteLater();
    update();
}

void DesktopWidget::searchWeatherCity()
{
    const QString currentLocation = AppSettings().value(
        QStringLiteral("weather/location"), m_weatherLocation).toString();
    const auto selection = WeatherCityDialog::choose(this, m_weatherNetwork, currentLocation);
    if (selection.cityId.isEmpty())
        return;
    AppSettings settings;
    settings.setValue(QStringLiteral("weather/location"), selection.name);
    settings.setValue(QStringLiteral("weather/cityId"), selection.cityId);
    for (DesktopWidget* widget : s_instances) {
        if (!widget || widget->m_cardKind != CardKind::Weather)
            continue;
        widget->m_weatherLocation = selection.name;
        widget->m_weatherCityId = selection.cityId;
        widget->refreshWeather();
    }
}
