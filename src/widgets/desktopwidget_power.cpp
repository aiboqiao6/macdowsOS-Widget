#include "widgets/desktopwidget.h"
#include "platform/powerdevices.h"
#include "widgets/cardmetrics.h"
#include <QElapsedTimer>
#include <QSystemTrayIcon>
#include <QtConcurrent/QtConcurrentRun>

void DesktopWidget::refreshBattery()
{
    const int previousLevel = m_batteryLevel;
    const bool previousCharging = m_charging;
    const bool previousHasBattery = m_hasBattery;
    const auto status = PowerDevices::powerStatus();
    m_hasBattery = status.hasBattery;
    m_charging = status.charging;
    m_batteryLevel = status.level;

    // 外设连接变化较慢，首次立即读取，之后每三十秒刷新。
    if (m_devicePollCount == 0)
        refreshConnectedDevices();
    m_devicePollCount = (m_devicePollCount + 1) % 15;

    const bool changed = previousLevel != m_batteryLevel
                         || previousCharging != m_charging
                         || previousHasBattery != m_hasBattery;
    // 电源状态未变化时跳过重绘和托盘图标更新。
    if (changed)
        update();
    if (changed && m_trayIcon) {
        m_trayIcon->setIcon(createTrayIcon());
        m_trayIcon->setToolTip(m_hasBattery
                                   ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_batteryLevel)
                                   : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    }
}

void DesktopWidget::refreshConnectedDevices()
{
    if (m_deviceWatcher.isRunning())
        return;
    // 设备发现可能阻塞数秒；电量卡片共用后台任务，结果交付界面线程，任务不捕获窗口指针。
    static QFuture<QVector<ConnectedDevice>> shared;
    static QElapsedTimer age;
    if (!age.isValid() || (age.elapsed() >= 30000 && shared.isFinished())) {
        shared = QtConcurrent::run(&PowerDevices::connectedDevices, CardMetrics::kMaxConnectedDevices);
        age.restart();
    }
    m_deviceWatcher.setFuture(shared);
}

