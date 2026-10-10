#include "media/systemmediasession.h"
#include "rendering/renderdiagnostics.h"

#include <QApplication>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#ifdef Q_OS_WIN
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#endif

struct SystemMediaSession::Backend {
#ifdef Q_OS_WIN
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    QString artworkKey;
    QImage artwork;
    int artworkRetryTicks = 0;
#endif

    MediaSnapshot read(Command command)
    {
        MediaSnapshot result;
#ifdef Q_OS_WIN
        using namespace winrt::Windows::Media::Control;
        using namespace winrt::Windows::Storage::Streams;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            // 每次调用平衡 COM 初始化；管理器和媒体会话为可跨单元使用的 WinRT 对象。
            struct ApartmentScope { ~ApartmentScope() { winrt::uninit_apartment(); } } apartment;
            if (!manager)
                manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            // 优先显示正在播放的会话，避免前台暂停的播放器遮住另一个播放源。
            if (!session || session.GetPlaybackInfo().PlaybackStatus()
                    != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                for (const auto& candidate : manager.GetSessions()) {
                    if (candidate.GetPlaybackInfo().PlaybackStatus()
                            == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                        session = candidate;
                        break;
                    }
                }
            }
            if (!session) {
                artworkKey.clear();
                artwork = {};
                return result;
            }
            auto playback = session.GetPlaybackInfo();
            const auto controls = playback.Controls();
            bool accepted = true;
            if (command == Command::TogglePlayback && controls.IsPlayPauseToggleEnabled())
                accepted = session.TryTogglePlayPauseAsync().get();
            else if (command == Command::Previous && controls.IsPreviousEnabled())
                accepted = session.TrySkipPreviousAsync().get();
            else if (command == Command::Next && controls.IsNextEnabled())
                accepted = session.TrySkipNextAsync().get();
            if (!accepted)
                result.error = QStringLiteral("播放器未接受控制请求");
            playback = session.GetPlaybackInfo();
            const auto properties = session.TryGetMediaPropertiesAsync().get();
            result.available = true;
            result.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            switch (playback.PlaybackStatus()) {
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
                result.playbackLabel = QStringLiteral("正在播放"); break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
                result.playbackLabel = QStringLiteral("已暂停"); break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
                result.playbackLabel = QStringLiteral("正在切换曲目"); break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
                result.playbackLabel = QStringLiteral("已停止"); break;
            default:
                result.playbackLabel = QStringLiteral("等待播放"); break;
            }
            result.canToggle = playback.Controls().IsPlayPauseToggleEnabled();
            result.canPrevious = playback.Controls().IsPreviousEnabled();
            result.canNext = playback.Controls().IsNextEnabled();
            result.title = QString::fromWCharArray(properties.Title().c_str());
            result.artist = QString::fromWCharArray(properties.Artist().c_str());
            result.source = QString::fromWCharArray(session.SourceAppUserModelId().c_str());
            const auto timeline = session.GetTimelineProperties();
            result.durationMs = qMax<qint64>(0, (timeline.EndTime() - timeline.StartTime()).count() / 10000);
            result.positionMs = qMax<qint64>(0, (timeline.Position() - timeline.StartTime()).count() / 10000);
            if (result.playing && result.durationMs > 0) {
                const auto elapsed = winrt::clock::now() - timeline.LastUpdatedTime();
                // 时间线通常只在跳转时更新，按播放速率推算两次系统更新之间的进度。
                const double rate = playback.PlaybackRate() ? playback.PlaybackRate().Value() : 1.0;
                result.positionMs += qMax<qint64>(0, qint64(elapsed.count() / 10000 * rate));
            }
            if (result.durationMs > 0)
                result.positionMs = qMin(result.positionMs, result.durationMs);
            const QString key = result.source + QChar('|') + result.title + QChar('|')
                + result.artist + QChar('|') + QString::fromWCharArray(properties.AlbumTitle().c_str());
            // 部分播放器晚于曲目信息发布封面；空封面每五次轮询重试。
            if (key != artworkKey || (artwork.isNull() && ++artworkRetryTicks >= 5)) {
                artworkRetryTicks = 0;
                artworkKey.clear();
                artwork = {};
                if (const auto thumbnail = properties.Thumbnail()) {
                    const auto stream = thumbnail.OpenReadAsync().get();
                    // 限制封面大小，防止异常播放器返回过大的流。
                    if (stream.Size() > 0 && stream.Size() <= 5 * 1024 * 1024) {
                        DataReader reader(stream.GetInputStreamAt(0));
                        const uint32_t size = reader.LoadAsync(uint32_t(stream.Size())).get();
                        QByteArray bytes(int(size), Qt::Uninitialized);
                        reader.ReadBytes(winrt::array_view<uint8_t>(
                            reinterpret_cast<uint8_t*>(bytes.data()),
                            reinterpret_cast<uint8_t*>(bytes.data()) + size));
                        artwork = QImage::fromData(bytes).scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                }
                artworkKey = key;
            }
            result.artwork = artwork;
        } catch (const winrt::hresult_error& error) {
            result.error = QStringLiteral("暂时无法读取系统媒体会话");
            manager = nullptr;
            artworkKey.clear();
            artwork = {};
            RenderDiagnostics::write("media.failure", QString("HRESULT=0x%1")
                .arg(quint32(error.code().value), 8, 16, QLatin1Char('0')), 10000);
        } catch (const std::exception&) {
            result.error = QStringLiteral("媒体信息读取失败，稍后重试");
        }
#else
        Q_UNUSED(command);
        result.error = QStringLiteral("系统媒体会话需要 Windows 10 1809 或更高版本");
#endif
        return result;
    }
};

SystemMediaSession* SystemMediaSession::instance()
{
    static QPointer<SystemMediaSession> service;
    if (!service)
        service = new SystemMediaSession(qApp);
    return service;
}

SystemMediaSession::SystemMediaSession(QObject* parent)
    : QObject(parent), m_backend(std::make_shared<Backend>())
{
    m_workerPool.setMaxThreadCount(1);
    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] { request(Command::Refresh); });
    connect(&m_watcher, &QFutureWatcher<MediaSnapshot>::finished, this, [this] {
        const MediaSnapshot next = m_watcher.result();
        const bool changedState = next.available != m_snapshot.available || next.playing != m_snapshot.playing
            || next.canToggle != m_snapshot.canToggle || next.canPrevious != m_snapshot.canPrevious
            || next.canNext != m_snapshot.canNext || next.title != m_snapshot.title
            || next.artist != m_snapshot.artist || next.source != m_snapshot.source
            || next.error != m_snapshot.error || next.playbackLabel != m_snapshot.playbackLabel
            || next.positionMs != m_snapshot.positionMs || next.durationMs != m_snapshot.durationMs
            || next.artwork.cacheKey() != m_snapshot.artwork.cacheKey();
        m_snapshot = next;
        if (changedState)
            emit changed();
        if (m_pendingCommand) {
            const Command command = *m_pendingCommand;
            m_pendingCommand.reset();
            request(command);
        }
    });
    m_pollTimer.start();
    request(Command::Refresh);
}

SystemMediaSession::~SystemMediaSession()
{
    m_pollTimer.stop();
    m_workerPool.waitForDone();
}

void SystemMediaSession::request(Command command)
{
    if (m_watcher.isRunning()) {
        if (command != Command::Refresh)
            m_pendingCommand = command;
        return;
    }
    const auto backend = m_backend;
    m_watcher.setFuture(QtConcurrent::run(&m_workerPool, [backend, command] {
        return backend->read(command);
    }));
}

void SystemMediaSession::sendCommand(Command command)
{
    // 轮询不排队；刷新期间点击控制时保留一个待执行动作，避免丢失点击。
    request(command);
}
