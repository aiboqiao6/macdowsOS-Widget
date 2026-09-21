#include "widgets/batterywidget.h"

#include <QApplication>
#include <QFontDatabase>
#include <QSettings>

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
    app.setOrganizationName(QStringLiteral("macdowsOS"));
    // All widget artwork and Qt controls use the bundled PingFang face. The
    // registration happens before any windows are restored so text metrics
    // cannot differ between the gallery and live cards.
    const QString pingFangFamily = BatteryWidget::pingFangFontFamily();
    QFont applicationFont(pingFangFamily);
    app.setFont(applicationFont);
    BatteryWidget::setGlobalFontSmoothing(qBound(0,
        QSettings().value(QStringLiteral("appearance/fontSmoothing"), 2).toInt(), 3));
    // The tray icon owns the app lifetime when the floating card is hidden.
    app.setQuitOnLastWindowClosed(false);

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
    const int result = app.exec();
    BatteryWidget::shutdown();
#ifdef Q_OS_WIN
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
#endif
    return result;
}
