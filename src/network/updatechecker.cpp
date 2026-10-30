#include "updatechecker.h"
#include "config.h"
#include "network/cardrequest.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>

UpdateChecker::UpdateChecker(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this))
{
}

QString UpdateChecker::buildDate(const QString& tag)
{
    static const QRegularExpression format(QStringLiteral("^([0-9]{8})"));
    const auto match = format.match(tag);
    const QString date = match.captured(1);
    return match.hasMatch() && QDate::fromString(date, QStringLiteral("yyyyMMdd")).isValid()
        ? date : QString();
}

void UpdateChecker::fail(const QString& message)
{
    m_state = State::Error;
    m_message = message;
    emit changed();
}

void UpdateChecker::check()
{
    if (m_state == State::Checking)
        return;
    m_state = State::Checking;
    m_message = QStringLiteral("正在检查更新…");
    m_releaseTag.clear();
    m_releaseName.clear();
    m_releaseNotes.clear();
    m_releaseUrl.clear();
    emit changed();
    // releases/latest 不包含测试版本；读取发布列表以覆盖当前 Beta 渠道。
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.github.com/repos/aiboqiao6/macdowsOS-Widget/releases?per_page=100")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("macdowsOS-Widget/%1").arg(QString::fromLatin1(AppConfig::BuildId)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = CardRequest::get(*m_network, request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 403 || status == 429) {
            fail(QStringLiteral("GitHub 请求次数已达上限，请稍后重试。"));
        } else if (reply->error() != QNetworkReply::NoError || status != 200) {
            fail(QStringLiteral("无法检查更新，请检查网络连接后重试。"));
        } else {
            readReleases(reply->readAll());
        }
    });
}

void UpdateChecker::readReleases(const QByteArray& json)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        fail(QStringLiteral("更新服务返回的数据无效，请稍后重试。"));
        return;
    }
    const QString localTag = QString::fromLatin1(AppConfig::BuildId);
    const QString localDate = buildDate(localTag);
    if (localDate.isEmpty()) {
        fail(QStringLiteral("当前版本号格式无效，无法比较更新。"));
        return;
    }
    QJsonObject newest;
    QString newestDate;
    for (const auto& value : document.array()) {
        const auto release = value.toObject();
        if (release.value(QStringLiteral("draft")).toBool())
            continue;
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        const QString date = buildDate(tag);
        if (date.isEmpty())
            continue;
        if (newest.isEmpty() || date > newestDate) {
            newest = release;
            newestDate = date;
        }
    }
    if (newest.isEmpty()) {
        fail(QStringLiteral("尚未找到可识别的发布版本，请稍后重试。"));
        return;
    }
    m_releaseTag = newest.value(QStringLiteral("tag_name")).toString();
    m_releaseName = newest.value(QStringLiteral("name")).toString().trimmed();
    if (m_releaseName.isEmpty())
        m_releaseName = QStringLiteral("macdowsOS Widget %1").arg(newestDate);
    m_releaseNotes = newest.value(QStringLiteral("body")).toString();
    // 下载按钮只打开本项目的 HTTPS 发布页面。
    m_releaseUrl = QUrl(QStringLiteral("https://github.com/aiboqiao6/macdowsOS-Widget/releases/tag/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(m_releaseTag))));
    if (newestDate > localDate) {
        m_state = State::Available;
        m_message = QStringLiteral("有可用更新");
    } else {
        m_state = State::Current;
        m_message = QStringLiteral("已是最新版本");
    }
    emit changed();
}
