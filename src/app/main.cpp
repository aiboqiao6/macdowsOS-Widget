#include "widgets/batterywidget.h"

#include <QApplication>
#include <QFontDatabase>

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
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE,
        L"Local\\macdowsOS.Widget.Singleton.1");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
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
    } else {
        for (BatteryWidget* widget : widgets)
            if (!widget->isVisible())
                widget->show();
    }
    const int result = app.exec();
#ifdef Q_OS_WIN
    if (instanceMutex) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
#endif
    return result;
}
