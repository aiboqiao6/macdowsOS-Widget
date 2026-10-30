#pragma once

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

class UpdateChecker final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Checking, Current, Available, Error };
    Q_ENUM(State)
    explicit UpdateChecker(QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    void check();
    State state() const { return m_state; }
    QString message() const { return m_message; }
    QString releaseName() const { return m_releaseName; }
    QString releaseTag() const { return m_releaseTag; }
    QString releaseNotes() const { return m_releaseNotes; }
    QUrl releaseUrl() const { return m_releaseUrl; }
    // 仅比较 Tag 和本地版本号的前八位日期，忽略后缀。
    static QString buildDate(const QString& tag);
    void readReleases(const QByteArray& json);
signals:
    void changed();
private:
    void fail(const QString& message);
    QNetworkAccessManager* m_network;
    State m_state = State::Idle;
    QString m_message, m_releaseName, m_releaseTag, m_releaseNotes;
    QUrl m_releaseUrl;
};
