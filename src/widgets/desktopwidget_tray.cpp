#include "widgets/desktopwidget.h"
#include "app/appsettings.h"
#include "rendering/renderdiagnostics.h"
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

QIcon DesktopWidget::createTrayIcon() const
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBatteryIcon(painter, QRectF(3, 7, 26, 18),
                    m_hasBattery ? m_batteryLevel : -1, m_charging);
    return QIcon(pixmap);
}

void DesktopWidget::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    auto* menu = new QMenu(this);
    menu->setStyleSheet(QStringLiteral(
        "QMenu { background:#1d2333; color:#f5f7ff; border:1px solid #4b5878; "
        "border-radius:8px; padding:5px; }"
        "QMenu::item { padding:6px 28px 6px 12px; border-radius:5px; }"
        "QMenu::item:selected { background:#36528a; }"
        "QMenu::separator { height:1px; background:#3a4560; margin:4px 8px; }"));

    m_visibilityAction = menu->addAction(QString());
    connect(m_visibilityAction, &QAction::triggered, this, &DesktopWidget::toggleWidgetVisibility);

    auto* libraryAction = menu->addAction(QStringLiteral("添加小组件…"));
    connect(libraryAction, &QAction::triggered, this, &DesktopWidget::showWidgetLibrary);

    m_rearrangeAction = menu->addAction(QString());
    connect(m_rearrangeAction, &QAction::triggered, this, &DesktopWidget::rearrangeWidgets);

    auto* settingsAction = menu->addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, &DesktopWidget::showSettingsDialog);

    m_startupAction = menu->addAction(QStringLiteral("开机自启动"));
    m_startupAction->setCheckable(true);
    m_startupAction->setChecked(isStartupEnabled());
    connect(m_startupAction, &QAction::toggled, this, &DesktopWidget::setStartupEnabled);

    menu->addSeparator();
    auto* logsAction = menu->addAction(QStringLiteral("打开渲染日志文件夹"));
    logsAction->setEnabled(!RenderDiagnostics::logPath().isEmpty());
    connect(logsAction, &QAction::triggered, this, []() {
        const QString path = RenderDiagnostics::logPath();
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    auto* aboutAction = menu->addAction(QStringLiteral("关于 macdowsOS Widget"));
    connect(aboutAction, &QAction::triggered, this, &DesktopWidget::showAboutDialog);
    menu->addSeparator();
    auto* quitAction = menu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, qApp, []() {
        if (confirmCustomWidgetEditorClose()) qApp->quit();
    });

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(createTrayIcon());
    m_trayIcon->setToolTip(m_hasBattery
                               ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_batteryLevel)
                               : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    m_trayIcon->setContextMenu(menu);
    m_trayClickTimer.setSingleShot(true);
    m_trayClickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_trayClickTimer, &QTimer::timeout, this, &DesktopWidget::toggleWidgetVisibility);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    m_trayClickTimer.start();
                else if (reason == QSystemTrayIcon::DoubleClick) {
                    m_trayClickTimer.stop();
                    toggleWidgetVisibility();
                }
            });
    m_trayIcon->show();
    updateTrayVisibilityLabel();
    updateRearrangeLabel();
}

void DesktopWidget::updateTrayVisibilityLabel()
{
    if (!m_visibilityAction)
        return;
    bool anyVisible = false;
    for (DesktopWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    m_visibilityAction->setText(anyVisible ? QStringLiteral("隐藏所有组件")
                                           : QStringLiteral("显示所有组件"));
}

void DesktopWidget::toggleWidgetVisibility()
{
    bool anyVisible = false;
    for (DesktopWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    for (DesktopWidget* widget : s_instances) {
        if (anyVisible) {
            widget->hide();
        } else {
            widget->show();
            widget->captureDesktopBackdrop();
        }
    }
    updateTrayVisibilityLabel();
    saveAllConfigurations();
}

void DesktopWidget::updateRearrangeLabel()
{
    if (m_rearrangeAction)
        m_rearrangeAction->setText(QStringLiteral("重新排列组件"));
}

void DesktopWidget::rearrangeWidgets()
{
    arrangeGroup();
    saveAllConfigurations();
    updateRearrangeLabel();
}
