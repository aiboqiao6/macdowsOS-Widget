#include "widgets/desktopwidget.h"
#include "network/cardrequest.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <QHash>
#include <QRandomGenerator>

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
    QNetworkReply* reply = CardRequest::get(*m_dictionaryNetwork, request);
    m_dictionaryReply = reply;
    reply->setProperty("dictionarySerial", m_dictionaryReplySerial);
    reply->setProperty("dictionaryWord", m_dictionaryWord);
    m_dictionaryPhonetic.clear();
    m_dictionaryPartOfSpeech.clear();
    m_dictionaryDefinition = QStringLiteral("读取在线释义中…");
    update();

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

