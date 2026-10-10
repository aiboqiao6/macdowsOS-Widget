#include "widgets/desktopwidget.h"
#include "widgets/weatherformat.h"
#include "app/appsettings.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDate>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QHBoxLayout>

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
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
    request.setRawHeader("Referer", "https://www.weather.com.cn/");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    m_weatherLoading = true;
    // 刷新天气期间保留最后完整快照，避免清空内容造成闪烁。
    if (m_weatherTemperature.isEmpty()) {
        m_weatherDescription = QStringLiteral("获取天气中…");
        update();
    }
    QNetworkReply* reply = m_weatherNetwork->get(request);
    m_weatherReply = reply;
    reply->setProperty("weatherSerial", m_weatherReplySerial);
    // 为网络请求设置超时，避免离线或认证网络中的回复长期存活。
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
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
        const QRegularExpression descriptionExpression(
            QStringLiteral(R"(<p\b[^>]*class\s*=\s*["'][^"']*\bwea\b[^"']*["'][^>]*>(.*?)</p>)"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
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
            const QRegularExpressionMatch descriptionMatch =
                descriptionExpression.match(card);
            const QRegularExpressionMatch temperatureMatch =
                temperatureExpression.match(card);
            if (!temperatureMatch.hasMatch())
                continue;
            WeatherDay item;
            item.label = chineseWeekdayLabel(QDate::currentDate().addDays(days.size() + 1));
            item.high = temperatureMatch.captured(1).trimmed();
            item.low = temperatureMatch.captured(2).trimmed();
            item.description = descriptionMatch.hasMatch()
                ? descriptionMatch.captured(1).simplified() : QString();
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
            item.description = fields.at(2).trimmed();
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
            item.description = day.value(QStringLiteral("fa")).toString().trimmed();
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
        m_weatherLoading = false;
        m_weatherUpdated = QDateTime::currentDateTime();

        QNetworkRequest hourlyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather1d/%1.shtml")
                .arg(m_weatherCityId)));
        hourlyRequest.setHeader(QNetworkRequest::UserAgentHeader,
                                QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
        hourlyRequest.setRawHeader("Referer", "https://www.weather.com.cn/");
        hourlyRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                   QNetworkRequest::AlwaysNetwork);
        QNetworkReply* hourlyReply = m_weatherNetwork->get(hourlyRequest);
        m_weatherReply = hourlyReply;
        hourlyReply->setProperty("weatherSerial", m_weatherReplySerial);
        hourlyReply->setProperty("weatherHourly", true);
        QTimer::singleShot(15000, hourlyReply, [hourlyReply]() {
            if (hourlyReply->isRunning())
                hourlyReply->abort();
        });

        // 首页预报天数不足时补充七日页面，以便从明天开始显示五行。
        QNetworkRequest dailyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather/%1.shtml")
                .arg(m_weatherCityId)));
        dailyRequest.setHeader(QNetworkRequest::UserAgentHeader,
                               QStringLiteral("Mozilla/5.0 macdowsOS Widget"));
        dailyRequest.setRawHeader("Referer", "https://www.weather.com.cn/");
        dailyRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                  QNetworkRequest::AlwaysNetwork);
        QNetworkReply* dailyReply = m_weatherNetwork->get(dailyRequest);
        dailyReply->setProperty("weatherSerial", m_weatherReplySerial);
        dailyReply->setProperty("weatherDaily", true);
        QTimer::singleShot(15000, dailyReply, [dailyReply]() {
            if (dailyReply->isRunning())
                dailyReply->abort();
        });
    } else {
        m_weatherLoading = false;
        m_weatherDescription = m_weatherTemperature.isEmpty()
            ? QStringLiteral("天气暂不可用") : QStringLiteral("更新失败 · 显示上次天气");
    }
    reply->deleteLater();
    update();
}

void DesktopWidget::refreshDictionary()
{
    if (!m_dictionaryNetwork)
        return;
    ++m_dictionaryReplySerial;
    if (m_dictionaryReply)
        m_dictionaryReply->abort();
    // 从精选词表随机选词，再向词典读取真实音标、词性与释义，减少额外服务依赖。
    static const QStringList words = {
        QStringLiteral("serendipity"), QStringLiteral("luminous"),
        QStringLiteral("wanderlust"), QStringLiteral("ephemeral"),
        QStringLiteral("resilient"), QStringLiteral("curiosity"),
        QStringLiteral("harmony"), QStringLiteral("diligent"),
        QStringLiteral("nostalgia"), QStringLiteral("vivid"),
        QStringLiteral("whisper"), QStringLiteral("breeze")
    };
    m_dictionaryWord = words.at(int(QRandomGenerator::global()->bounded(words.size())));
    QNetworkRequest request(QUrl(QStringLiteral("https://api.dictionaryapi.dev/api/v2/entries/en/%1")
                                  .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_dictionaryWord)))));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("macdowsOS Widget/1.0 (Qt)"));
    QNetworkReply* reply = m_dictionaryNetwork->get(request);
    m_dictionaryReply = reply;
    reply->setProperty("dictionarySerial", m_dictionaryReplySerial);
    reply->setProperty("dictionaryWord", m_dictionaryWord);
    m_dictionaryPhonetic.clear();
    m_dictionaryPartOfSpeech.clear();
    m_dictionaryDefinition = QStringLiteral("读取在线释义中…");
    update();
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
}

void DesktopWidget::handleDictionaryReply(QNetworkReply* reply)
{
    if (!reply)
        return;
    const int serial = reply->property("dictionarySerial").toInt();
    if (serial != m_dictionaryReplySerial) {
        reply->deleteLater();
        return;
    }
    const QString word = reply->property("dictionaryWord").toString();
    m_dictionaryDefinition.clear();
    bool ok = reply->error() == QNetworkReply::NoError;
    const QJsonDocument document = ok ? QJsonDocument::fromJson(reply->readAll()) : QJsonDocument();
    const QJsonArray entries = document.isArray() ? document.array() : QJsonArray();
    if (ok && !entries.isEmpty()) {
        const QJsonObject entry = entries.first().toObject();
        m_dictionaryPhonetic = entry.value(QStringLiteral("phonetic")).toString();
        const QJsonArray phonetics = entry.value(QStringLiteral("phonetics")).toArray();
        if (m_dictionaryPhonetic.isEmpty() && !phonetics.isEmpty())
            m_dictionaryPhonetic = phonetics.first().toObject().value(QStringLiteral("text")).toString();
        const QJsonArray meanings = entry.value(QStringLiteral("meanings")).toArray();
        if (!meanings.isEmpty()) {
            const QJsonObject meaning = meanings.first().toObject();
            m_dictionaryPartOfSpeech = meaning.value(QStringLiteral("partOfSpeech")).toString();
            const QJsonArray definitions = meaning.value(QStringLiteral("definitions")).toArray();
            if (!definitions.isEmpty())
                m_dictionaryDefinition = definitions.first().toObject()
                                             .value(QStringLiteral("definition")).toString();
        }
    }
    if (!ok || m_dictionaryDefinition.isEmpty()) {
        static const QHash<QString, QString> localDefinitions = {
            {QStringLiteral("breeze"), QStringLiteral("微风；轻柔的风")},
            {QStringLiteral("diligent"), QStringLiteral("勤奋的；孜孜不倦的")},
            {QStringLiteral("serendipity"), QStringLiteral("意外发现美好事物的运气")},
            {QStringLiteral("luminous"), QStringLiteral("发光的；明亮的")},
            {QStringLiteral("wanderlust"), QStringLiteral("旅行癖；对远方的向往")},
            {QStringLiteral("ephemeral"), QStringLiteral("短暂的；转瞬即逝的")},
            {QStringLiteral("resilient"), QStringLiteral("有韧性的；能迅速恢复的")},
            {QStringLiteral("curiosity"), QStringLiteral("好奇心；求知欲")},
            {QStringLiteral("harmony"), QStringLiteral("和谐；协调")},
            {QStringLiteral("nostalgia"), QStringLiteral("怀旧；对往昔的眷恋")},
            {QStringLiteral("vivid"), QStringLiteral("生动的；鲜明的")},
            {QStringLiteral("whisper"), QStringLiteral("低语；轻声说话")}
        };
        m_dictionaryDefinition = localDefinitions.value(
            m_dictionaryWord, QStringLiteral("释义暂不可用，可点击“随机单词”重试"));
        if (m_dictionaryPartOfSpeech.isEmpty())
            m_dictionaryPartOfSpeech = QStringLiteral("词义速览");
    }
    if (!word.isEmpty())
        m_dictionaryWord = word;
    reply->deleteLater();
    update();
}

void DesktopWidget::searchWeatherCity()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("搜索城市或区县"));
    dialog.setModal(true);
    dialog.resize(520, 440);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("搜索城市、区或县"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    layout->addWidget(title);
    auto* hint = new QLabel(QStringLiteral("输入简体中文名称，可精确选择区县；例如“海淀”“天心”“浦东新区”。"),
                            &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    layout->addWidget(hint);
    auto* searchRow = new QWidget(&dialog);
    auto* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(8);
    auto* edit = new QLineEdit(
        AppSettings().value(QStringLiteral("weather/location"), m_weatherLocation).toString(),
        searchRow);
    edit->setPlaceholderText(QStringLiteral("输入城市、区或县"));
    edit->selectAll();
    auto* searchButton = new QPushButton(QStringLiteral("搜索"), searchRow);
    searchLayout->addWidget(edit, 1);
    searchLayout->addWidget(searchButton);
    layout->addWidget(searchRow);
    auto* resultLabel = new QLabel(QStringLiteral("输入名称后点击搜索"), &dialog);
    resultLabel->setStyleSheet(QStringLiteral("color:#aeb9cb;"));
    layout->addWidget(resultLabel);
    auto* results = new QListWidget(&dialog);
    results->setAlternatingRowColors(false);
    layout->addWidget(results, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* chooseButton = buttons->addButton(QStringLiteral("使用所选地区"),
                                             QDialogButtonBox::AcceptRole);
    chooseButton->setEnabled(false);
    layout->addWidget(buttons);
    // 非天气卡片打开搜索时也需提供独立网络管理器。
    auto* searchNetwork = m_weatherNetwork ? m_weatherNetwork
                                           : new QNetworkAccessManager(&dialog);

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(results, &QListWidget::itemSelectionChanged, &dialog,
            [results, chooseButton]() { chooseButton->setEnabled(results->currentItem()); });
    connect(results, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem*) { dialog.accept(); });
    connect(chooseButton, &QPushButton::clicked, &dialog, [&dialog, results]() {
        if (results->currentItem())
            dialog.accept();
    });

    QPointer<QNetworkReply> searchReply;
    const auto performSearch = [searchNetwork, edit, results, resultLabel, searchButton,
                                chooseButton, &searchReply]() {
        const QString originalQuery = edit->text().trimmed();
        if (originalQuery.isEmpty()) {
            resultLabel->setText(QStringLiteral("请输入城市、区或县名称"));
            return;
        }
        QString query = originalQuery;
        if (query.size() > 1 && query.endsWith(QStringLiteral("市")))
            query.chop(1);
        if (query.size() > 1 && query.endsWith(QStringLiteral("区"))
            && !query.endsWith(QStringLiteral("新区")))
            query.chop(1);
        results->clear();
        chooseButton->setEnabled(false);
        resultLabel->setText(QStringLiteral("正在搜索…"));
        searchButton->setEnabled(false);
        if (searchReply)
            searchReply->abort();
        QUrl url(QStringLiteral("https://toy1.weather.com.cn/search"));
        QUrlQuery urlQuery;
        urlQuery.addQueryItem(QStringLiteral("cityname"), query);
        urlQuery.addQueryItem(QStringLiteral("_"),
                              QString::number(QDateTime::currentMSecsSinceEpoch()));
        url.setQuery(urlQuery);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
        request.setRawHeader("Referer", "https://www.weather.com.cn/");
        request.setTransferTimeout(15000);
        QNetworkReply* issuedReply = searchNetwork->get(request);
        searchReply = issuedReply;
        issuedReply->setProperty("weatherSearch", true);
        connect(issuedReply, &QNetworkReply::finished, results,
                [results, resultLabel, searchButton, chooseButton,
                 issuedReply, originalQuery, &searchReply]() {
            if (searchReply.data() != issuedReply) {
                issuedReply->deleteLater();
                return;
            }
            QNetworkReply* reply = issuedReply;
            searchReply = nullptr;
            const QByteArray payload = reply->readAll();
            const int open = payload.indexOf('[');
            const int close = payload.lastIndexOf(']');
            QJsonDocument document;
            if (reply->error() == QNetworkReply::NoError && open >= 0 && close > open)
                document = QJsonDocument::fromJson(payload.mid(open, close - open + 1));
            reply->deleteLater();
            const QJsonArray array = document.isArray() ? document.array() : QJsonArray{};
            QSet<QString> seen;
            for (const QJsonValue& value : array) {
                const QStringList fields = value.toObject().value(QStringLiteral("ref"))
                                               .toString().split(QLatin1Char('~'));
                if (fields.size() < 10)
                    continue;
                const QString cityId = fields.at(0).trimmed();
                const QString name = fields.at(2).trimmed();
                const QString parent = fields.at(9).trimmed();
                if (cityId.isEmpty() || name.isEmpty() || seen.contains(cityId))
                    continue;
                // 只接受官方城市与区县编号，排除乡镇和景点编号。
                if (cityId.size() != 9 || cityId.endsWith(QLatin1Char('A')))
                    continue;
                QString displayName = name;
                // 接口可能省略行政区后缀，保留用户搜索的区县全称以免刷新后缩短。
                if (cityId.size() == 9 && originalQuery.endsWith(QStringLiteral("区"))
                    && !displayName.endsWith(QStringLiteral("区")))
                    displayName += QStringLiteral("区");
                else if (cityId.size() == 9 && originalQuery.endsWith(QStringLiteral("县"))
                         && !displayName.endsWith(QStringLiteral("县")))
                    displayName += QStringLiteral("县");
                auto* item = new QListWidgetItem(
                    parent.isEmpty() ? displayName
                                     : QStringLiteral("%1  ·  %2").arg(displayName, parent), results);
                item->setData(Qt::UserRole, cityId);
                item->setData(Qt::UserRole + 1, displayName);
                seen.insert(cityId);
                if (results->count() == 30)
                    break;
            }
            searchButton->setEnabled(true);
            if (results->count() > 0) {
                results->setCurrentRow(0);
                chooseButton->setEnabled(true);
                resultLabel->setText(QStringLiteral("找到 %1 个结果，请选择准确地区")
                                         .arg(results->count()));
            } else {
                resultLabel->setText(QStringLiteral("没有找到区县级结果，请尝试去掉“市/区”后缀或输入上级城市"));
            }
        });
    };
    connect(searchButton, &QPushButton::clicked, &dialog, performSearch);
    connect(edit, &QLineEdit::returnPressed, &dialog, performSearch);

    const int result = dialog.exec();
    if (searchReply) {
        // abort 可能同步触发完成信号并清空指针，先解绑再通过局部保护指针取消。
        const QPointer<QNetworkReply> pending = searchReply;
        searchReply = nullptr;
        pending->abort();
        if (pending)
            pending->deleteLater();
    }
    if (result != QDialog::Accepted || !results->currentItem())
        return;
    const QString cityId = results->currentItem()->data(Qt::UserRole).toString();
    const QString city = results->currentItem()->data(Qt::UserRole + 1).toString();
    if (cityId.isEmpty() || city.isEmpty())
        return;
    AppSettings settings;
    settings.setValue(QStringLiteral("weather/location"), city);
    settings.setValue(QStringLiteral("weather/cityId"), cityId);
    for (DesktopWidget* widget : s_instances) {
        if (!widget || widget->m_cardKind != CardKind::Weather)
            continue;
        widget->m_weatherLocation = city;
        widget->m_weatherCityId = cityId;
        widget->refreshWeather();
    }
}
