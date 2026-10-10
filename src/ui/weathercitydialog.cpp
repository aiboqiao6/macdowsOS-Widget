#include "ui/weathercitydialog.h"
#include "network/cardrequest.h"
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QHBoxLayout>

WeatherCityDialog::Selection WeatherCityDialog::choose(QWidget* parent, QNetworkAccessManager* network,
                                                       const QString& currentLocation)
{
    QDialog dialog(parent);
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
    auto* edit = new QLineEdit(currentLocation, searchRow);
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
    auto* searchNetwork = network ? network
                                           : new QNetworkAccessManager(&dialog);

    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(results, &QListWidget::itemSelectionChanged, &dialog,
            [results, chooseButton]() { chooseButton->setEnabled(results->currentItem()); });
    QObject::connect(results, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem*) { dialog.accept(); });
    QObject::connect(chooseButton, &QPushButton::clicked, &dialog, [&dialog, results]() {
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
        CardRequest::configureWeather(request);
        QNetworkReply* issuedReply = CardRequest::get(*searchNetwork, request);
        searchReply = issuedReply;
        issuedReply->setProperty("weatherSearch", true);
        QObject::connect(issuedReply, &QNetworkReply::finished, results,
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
                if (originalQuery.endsWith(QStringLiteral("区"))
                    && !displayName.endsWith(QStringLiteral("区")))
                    displayName += QStringLiteral("区");
                else if (originalQuery.endsWith(QStringLiteral("县"))
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
    QObject::connect(searchButton, &QPushButton::clicked, &dialog, performSearch);
    QObject::connect(edit, &QLineEdit::returnPressed, &dialog, performSearch);

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
        return {};
    const QString cityId = results->currentItem()->data(Qt::UserRole).toString();
    const QString city = results->currentItem()->data(Qt::UserRole + 1).toString();
    if (cityId.isEmpty() || city.isEmpty())
        return {};
    return {cityId, city};
}
