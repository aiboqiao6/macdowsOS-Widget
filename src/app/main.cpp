#include "widgets/batterywidget.h"
#include "ui/firstrunwizard.h"
#include "app/appsettings.h"
#include "config.h"
#include "rendering/renderdiagnostics.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
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
    // One process owns the tray, configuration and desktop grid. A second
    // process would otherwise create duplicate widgets in the same cells and
    // race while writing widgets.json.
    // Use the global namespace so launches from different Windows sessions
    // (or from an elevated shortcut and a normal shortcut) still share one
    // process guard. Failing closed is important: if the guard cannot be
    // created, starting anyway would violate the single-instance contract.
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
    // This policy must be selected before QApplication creates the platform
    // integration; otherwise Qt ignores it and high-DPI framebuffer rounding
    // can differ from the desktop capture size.
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
    // All widget artwork and Qt controls use the bundled PingFang face. The
    // registration happens before any windows are restored so text metrics
    // cannot differ between the gallery and live cards.
    const QString pingFangFamily = BatteryWidget::pingFangFontFamily();
    QFont applicationFont(pingFangFamily);
    app.setFont(applicationFont);
    // The tray icon owns the app lifetime when the floating card is hidden.
    app.setQuitOnLastWindowClosed(false);

    const QString configPath = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("widgets.json"));
    const bool firstRun = !QFileInfo::exists(configPath);
    if (firstRun) {
        FirstRunWizard wizard;
        if (!wizard.run()) {
            BatteryWidget::shutdown();
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
        const AppSettings saved;
        RenderDiagnostics::write("render.settings", QString("renderScale=%1 liveBackdrop=%2 lowPower=%3 mouseThrough=%4 opacity=%5 blur=%6 fontSmoothing=%7")
            .arg(saved.value("performance/renderScale").toDouble())
            .arg(saved.value("appearance/liveBackdrop").toBool())
            .arg(saved.value("interaction/lowPowerRefresh").toBool())
            .arg(saved.value("interaction/mouseThrough").toBool())
            .arg(saved.value("appearance/opacity").toDouble())
            .arg(saved.value("appearance/blurStrength").toInt())
            .arg(saved.value("appearance/fontSmoothing").toInt()));
        BatteryWidget::setGlobalFontSmoothing(qBound(0,
            saved.value(QStringLiteral("appearance/fontSmoothing"), 2).toInt(), 3));
    }

    // Restore user-created independent cards from widgets.json. On first run
    // seed the reference layout with four cards; every card owns its window,
    // position and glass surface independently.
    QList<BatteryWidget*> widgets = BatteryWidget::restoreWidgets();
    if (widgets.isEmpty()) {
        widgets.append(new BatteryWidget(nullptr, BatteryWidget::CardKind::Battery, true));
        widgets.append(new BatteryWidget(nullptr, BatteryWidget::CardKind::Weather, false));
        widgets.append(new BatteryWidget(nullptr, BatteryWidget::CardKind::Clock, false));
        widgets.append(new BatteryWidget(nullptr, BatteryWidget::CardKind::Dictionary, false));
        for (BatteryWidget* widget : widgets)
            widget->show();
        widgets.first()->saveConfiguration();
    }
    if (firstRun) {
        BatteryWidget* owner = widgets.first();
        QTimer::singleShot(0, owner, &BatteryWidget::showSettingsDialog);
    }
    const int result = app.exec();
    BatteryWidget::shutdown();
    RenderDiagnostics::write("session.exit", QString("exitCode=%1").arg(result));
    RenderDiagnostics::shutdown();
#ifdef Q_OS_WIN
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
#endif
    return result;
}
