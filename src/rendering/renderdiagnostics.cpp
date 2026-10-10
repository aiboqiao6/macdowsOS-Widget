#include "renderdiagnostics.h"
#include "config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <atomic>
#include <cstdio>

namespace {
constexpr qint64 maximumBytes = 2 * 1024 * 1024;
struct Rate {
    qint64 last = -1;
    quint64 suppressed = 0;
};
struct LogState {
    QMutex mutex;
    QFile file;
    QElapsedTimer clock;
    QHash<QString, Rate> rates;
    std::atomic<bool> active{false};
    std::atomic<QtMessageHandler> previous{nullptr};
    bool handlerInstalled = false;
};
LogState& state() {
    // Qt may still emit platform teardown warnings after QApplication dies.
    // Keep the handler state alive until process exit.
    static auto* value = new LogState;
    return *value;
}
bool rotate(LogState& log) {
    const QString path = log.file.fileName();
    log.file.close();
    if (QFile::exists(path + ".3") && !QFile::remove(path + ".3"))
        return false;
    for (int index = 2; index >= 1; --index) {
        const QString from = path + '.' + QString::number(index);
        if (QFile::exists(from) && !QFile::rename(from, path + '.' + QString::number(index + 1)))
            return false;
    }
    if (QFile::exists(path) && !QFile::rename(path, path + ".1"))
        return false;
    return log.file.open(QIODevice::WriteOnly | QIODevice::Append);
}
void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QString category = QString::fromUtf8(context.category ? context.category : "default");
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg
        || category.startsWith("qt.qpa") || category.startsWith("qt.rhi")
        || category.startsWith("qt.opengl")) {
        RenderDiagnostics::write("qt." + category + '.' + QString::number(int(type)), message, 1000);
    }
    if (const auto previous = state().previous.load()) {
        previous(type, context, message);
    } else {
        const QByteArray formatted = qFormatLogMessage(type, context, message).toLocal8Bit();
        std::fprintf(stderr, "%s\n", formatted.constData());
    }
}
}

bool RenderDiagnostics::initialize(const QString& directory) {
    shutdown();
    const QString selected = directory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("logs")
        : directory;
    if (!QDir().mkpath(selected))
        return false;
    auto& log = state();
    {
        QMutexLocker lock(&log.mutex);
        log.file.setFileName(QDir(selected).filePath("render.log"));
        if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append))
            return false;
        log.clock.start();
        log.rates.clear();
        log.active.store(true);
        log.previous.store(qInstallMessageHandler(&messageHandler));
        log.handlerInstalled = true;
    }
    write("session.start", QString("version=%1 build=%2 pid=%3 Qt=%4 os=%5 kernel=%6 arch=%7 QT_OPENGL=%8 QT_WIDGETS_RHI=%9 timestamps=UTC")
        .arg(AppConfig::Version, AppConfig::BuildId).arg(QCoreApplication::applicationPid())
        .arg(qVersion(), QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
             QSysInfo::currentCpuArchitecture(), qEnvironmentVariable("QT_OPENGL"),
             qEnvironmentVariable("QT_WIDGETS_RHI")));
    return true;
}

void RenderDiagnostics::shutdown() {
    auto& log = state();
    QMutexLocker lock(&log.mutex);
    if (log.handlerInstalled) {
        qInstallMessageHandler(log.previous.load());
        log.handlerInstalled = false;
    }
    log.active.store(false);
    log.file.close();
    log.file.setFileName({});
    log.rates.clear();
}

bool RenderDiagnostics::enabled() { return state().active.load(); }

QString RenderDiagnostics::logPath() {
    QMutexLocker lock(&state().mutex);
    return state().file.fileName();
}

void RenderDiagnostics::write(const QString& event, const QString& details, int minimumIntervalMs) {
    auto& log = state();
    if (!log.active.load())
        return;
    QMutexLocker lock(&log.mutex);
    if (!log.active.load())
        return;
    quint64 suppressed = 0;
    if (minimumIntervalMs > 0) {
        if (log.rates.size() >= 512 && !log.rates.contains(event))
            log.rates.clear();
        auto& rate = log.rates[event];
        const qint64 now = log.clock.elapsed();
        if (rate.last >= 0 && now - rate.last < minimumIntervalMs) {
            ++rate.suppressed;
            return;
        }
        suppressed = rate.suppressed;
        rate = {now, 0};
    }
    QString safeDetails = details.left(4096);
    safeDetails.replace('\r', "\\r").replace('\n', "\\n");
    QString safeEvent = event.left(256);
    safeEvent.replace('\r', '_').replace('\n', '_');
    const QByteArray record = QString("%1 pid=%2 thread=0x%3 [%4] %5%6\n")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
        .arg(QCoreApplication::applicationPid())
        .arg(quintptr(QThread::currentThreadId()), 0, 16)
        .arg(safeEvent, safeDetails, suppressed ? QString(" suppressed=%1").arg(suppressed) : QString())
        .toUtf8();
    if (log.file.size() + record.size() > maximumBytes && !rotate(log)) {
        log.active.store(false);
        return;
    }
    if (log.file.write(record) != record.size() || !log.file.flush())
        log.active.store(false);
}
