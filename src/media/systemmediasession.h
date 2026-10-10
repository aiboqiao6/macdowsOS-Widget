#pragma once

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <memory>
#include <optional>

// 系统媒体会话的界面快照；不依赖播放器进程名或窗口标题。
struct MediaSnapshot {
    QString title;
    QString artist;
    QString source;
    QString playbackLabel;
    QString error;
    QImage artwork;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    bool available = false;
    bool playing = false;
    bool canToggle = false;
    bool canPrevious = false;
    bool canNext = false;
};

class SystemMediaSession final : public QObject
{
    Q_OBJECT
public:
    enum class Command { Refresh, TogglePlayback, Previous, Next };
    static SystemMediaSession* instance();
    const MediaSnapshot& snapshot() const { return m_snapshot; }
    void sendCommand(Command command);
    ~SystemMediaSession() override;

signals:
    void changed();

private:
    explicit SystemMediaSession(QObject* parent);
    struct Backend;
    void request(Command command);
    MediaSnapshot m_snapshot;
    QTimer m_pollTimer;
    QFutureWatcher<MediaSnapshot> m_watcher;
    // 串行工作队列避免刷新和播放控制互相覆盖，所有 WinRT 等待都在工作线程执行。
    QThreadPool m_workerPool;
    std::shared_ptr<Backend> m_backend;
    std::optional<Command> m_pendingCommand;
};
