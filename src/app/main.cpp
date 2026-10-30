#include "widgets/desktopwidget.h"
#include "ui/firstrunwizard.h"
#include "app/appsettings.h"
#include "config.h"
#include "rendering/renderdiagnostics.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QScreen>
#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    // 全局互斥量保证托盘与配置只有一个进程管理；创建失败时退出，避免重复组件及并发写入。
    SetLastError(ERROR_SUCCESS);
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE,
        L"Global\\macdowsOS.Widget.Singleton.1");
    if (!instanceMutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }
#endif
    // 必须在创建 QApplication 前启用小数缩放，保持 Qt 帧缓冲与桌面捕获的像素尺寸一致。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("macdowsOS Widget"));
    app.setApplicationDisplayName(QStringLiteral("macdowsOS Widget"));
    app.setApplicationVersion(QString::fromLatin1(AppConfig::Version));
    app.setOrganizationName(QStringLiteral("macdowsOS"));
    if (!RenderDiagnostics::initialize())
        qWarning("Could not open the local rendering diagnostic log");
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect area = screen->geometry();
        RenderDiagnostics::write("display", QString("name=%1 geometry=%2,%3,%4x%5 DPR=%6 refreshHz=%7 depth=%8")
            .arg(screen->name()).arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height())
            .arg(screen->devicePixelRatio()).arg(screen->refreshRate()).arg(screen->depth()));
    }
#ifdef Q_OS_WIN
    for (DWORD index = 0; ; ++index) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0))
            break;
        RenderDiagnostics::write("display.adapter", QString("name=%1 GPU=%2 device=%3 flags=0x%4")
            .arg(QString::fromWCharArray(adapter.DeviceName), QString::fromWCharArray(adapter.DeviceString),
                 QString::fromWCharArray(adapter.DeviceID)).arg(adapter.StateFlags, 0, 16));
    }
#endif
    // 恢复窗口前注册内置苹方字体，确保控件、图库和桌面组件使用一致的字形度量。
    const QString pingFangFamily = DesktopWidget::pingFangFontFamily();
    QFont applicationFont(pingFangFamily);
    DesktopWidget::applyFontSmoothing(applicationFont);
    app.setFont(applicationFont);
    // 组件隐藏后由托盘维持应用生命周期。
    app.setQuitOnLastWindowClosed(false);

    const QString configPath = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("widgets.json"));
    const bool firstRun = !QFileInfo::exists(configPath);
    if (firstRun) {
        FirstRunWizard wizard;
        if (!wizard.run()) {
            DesktopWidget::shutdown();
            RenderDiagnostics::write("session.exit", "reason=first-run-cancelled exitCode=0");
            RenderDiagnostics::shutdown();
#ifdef Q_OS_WIN
            ReleaseMutex(instanceMutex);
            CloseHandle(instanceMutex);
#endif
            return 0;
        }
    }
    {
        // Font smoothing is now always enabled; retire the saved off/profile choice.
        DesktopWidget::setGlobalFontSmoothing(4);
        const AppSettings saved;
        RenderDiagnostics::write("render.settings", QString("renderScale=%1 liveBackdrop=%2 lowPower=%3 mouseThrough=%4 material=liquidGlass fontSmoothing=%5")
            .arg(saved.value("performance/renderScale").toDouble())
            .arg(saved.value("appearance/liveBackdrop").toBool())
            .arg(saved.value("interaction/lowPowerRefresh").toBool())
            .arg(saved.value("interaction/mouseThrough").toBool())
            .arg(saved.value("appearance/fontSmoothing").toInt()));
    }

    // 恢复独立组件；没有配置时创建四种默认卡片，每个窗口独立保存位置。
    QList<DesktopWidget*> widgets = DesktopWidget::restoreWidgets();
    if (widgets.isEmpty()) {
        widgets.append(new DesktopWidget(nullptr, DesktopWidget::CardKind::Battery, true));
        widgets.append(new DesktopWidget(nullptr, DesktopWidget::CardKind::Weather, false));
        widgets.append(new DesktopWidget(nullptr, DesktopWidget::CardKind::Clock, false));
        widgets.append(new DesktopWidget(nullptr, DesktopWidget::CardKind::Dictionary, false));
        for (DesktopWidget* widget : widgets)
            widget->show();
        widgets.first()->saveConfiguration();
    }
    if (firstRun) {
        DesktopWidget* owner = widgets.first();
        QTimer::singleShot(0, owner, &DesktopWidget::showSettingsDialog);
    }
    const int result = app.exec();
    DesktopWidget::shutdown();
    RenderDiagnostics::write("session.exit", QString("exitCode=%1").arg(result));
    RenderDiagnostics::shutdown();
#ifdef Q_OS_WIN
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
#endif
    return result;
}
