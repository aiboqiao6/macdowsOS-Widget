#include "widgets/desktopwidget.h"
#include "ui/widgetlibrarydialog.h"
#include "ui/liquidglassmenu.h"
#include "widgets/musiccard.h"
#include <QMouseEvent>
#include <QMoveEvent>
#include <QContextMenuEvent>
#include <QAction>
#include <QToolTip>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QSystemTrayIcon>
#include <QtMath>
#include <utility>

void DesktopWidget::showWidgetLibrary()
{
    // 所有卡片共用托盘管理者的图库，避免重复创建置顶捕获面板。
    if (!m_ownsTray) {
        for (DesktopWidget* widget : std::as_const(s_instances)) {
            if (widget && widget != this && widget->m_ownsTray) {
                widget->showWidgetLibrary();
                return;
            }
        }
    }
    if (!m_library) {
        // 置顶工具窗口不能由桌面卡片原生拥有。
        m_library = new WidgetLibraryDialog;
        connect(m_library, &WidgetLibraryDialog::widgetDropped, this,
                [](int encoded, const QPoint& globalPos) {
                    const CardKind kind = static_cast<CardKind>(qBound(0, encoded / 100, int(CardKind::Countdown)));
                    const int variant = encoded % 100;
                    if (!addWidget(kind, globalPos, variant))
                        QToolTip::showText(globalPos, QStringLiteral("当前屏幕没有足够的空位，可缩小组件后重试"));
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragPreview, this,
                [this](int encoded, const QPoint& globalPos, bool visible) {
                    if (visible)
                        showGridPreview(static_cast<CardKind>(qBound(0, encoded / 100, int(CardKind::Custom))),
                                        encoded % 100, globalPos);
                    else
                        hideGridPreview();
                });
        connect(m_library, &WidgetLibraryDialog::customWidgetDropped, this,
                [](const QString& definitionId, const QPoint& globalPos) {
                    QString error;
                    const auto definitions = CustomWidgets::load(&error);
                    for (const auto& definition : definitions) {
                        if (definition.id != definitionId)
                            continue;
                        if (!addCustomWidget(definition, globalPos))
                            QToolTip::showText(globalPos, QStringLiteral("当前屏幕没有足够的空位，可缩小组件后重试"));
                        return;
                    }
                    QToolTip::showText(globalPos, error.isEmpty()
                        ? QStringLiteral("此自定义组件已从组件库移除，请重新选择") : error);
                    QTimer::singleShot(0, qApp, &WidgetLibraryDialog::refreshCustomWidgets);
                });
    }
    // 图库可能比设置窗口存活更久，每次打开时重新同步全局显示配置。
    m_library->updateScreenLayout();
    m_library->reloadCustomWidgets();
    m_library->setLiveBackdropEnabled(liveBackdropEnabled());
    m_library->setRenderScale(renderScale());
    if (!m_library->isVisible()) {
        m_library->prepareBackdrop();
        m_library->show();
    }
    m_library->raise();
    m_library->activateWindow();
}

void DesktopWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_cardKind == CardKind::Custom) {
            openCustomWidgetEditor(m_custom.id);
            event->accept();
            return;
        }
        if (m_cardKind == CardKind::Countdown) {
            editCountdown();
            event->accept();
            return;
        }
        if (m_cardKind == CardKind::Music) {
            event->accept();
            return;
        }
        if (m_cardKind == CardKind::Weather) {
            // 双击地点打开城市搜索，右键菜单提供同一入口。
            searchWeatherCity();
            event->accept();
            return;
        }
        // 主卡片双击重新排列组件，与托盘排列动作一致。
        if (m_ownsTray)
            arrangeGroup();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void DesktopWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragOrigin = pos();
        m_clickPending = true;
    }
    LiquidGlassWidget::mousePressEvent(event);
}

void DesktopWidget::windowDragStarted()
{
    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    QSet<WId> widgetWindows;
    for (DesktopWidget* widget : std::as_const(s_instances)) {
        if (!widget || !widget->isVisible())
            continue;
        widgetWindows.insert(widget->winId());
    }
    m_dragHiddenWindows = NativeWindows::hideApplicationWindows(widgetWindows);
}

void DesktopWidget::windowDragFinished()
{
    m_clickPending = false;
    hideGridPreview();
    bool shouldSave = false;
    if (!m_restoringPosition) {
        const QRect target = nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this);
        m_restoringPosition = true;
        if (target.isValid()) {
            setFixedSize(target.size());
            move(target.topLeft());
        } else if (overlapsVisibleWidget(geometry(), this)) {
            move(m_dragOrigin);
        }
        m_restoringPosition = false;
        m_hasSavedPosition = true;
        shouldSave = true;
    }

    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    // 先恢复普通应用窗口，再保存最终布局。
    if (shouldSave)
        saveAllConfigurations();
}

void DesktopWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const bool clicked = event->button() == Qt::LeftButton && m_clickPending
                         && !windowDragging() && !mouseThroughEnabled();
    m_clickPending = false;
    LiquidGlassWidget::mouseReleaseEvent(event);
    if (!clicked)
        return;
    const QPointF point = event->position() / m_widgetScale;
    if (m_cardKind == CardKind::Music) {
        const auto regions = MusicCard::layout(QRectF(0, 0, width() / m_widgetScale, height() / m_widgetScale));
        auto* media = SystemMediaSession::instance();
        const auto& state = media->snapshot();
        if (state.canToggle && regions.toggle.contains(point))
            media->sendCommand(SystemMediaSession::Command::TogglePlayback);
        else if (state.canPrevious && regions.previous.contains(point))
            media->sendCommand(SystemMediaSession::Command::Previous);
        else if (state.canNext && regions.next.contains(point))
            media->sendCommand(SystemMediaSession::Command::Next);
        return;
    }
    if (m_cardKind == CardKind::Dictionary
        && QRectF(32, height() / m_widgetScale - 52, 82, 32).contains(point))
        refreshDictionary();
    else if (m_cardKind == CardKind::Weather
             && QRectF(36, 27, width() / m_widgetScale - 64, 32).contains(point))
        searchWeatherCity();
}

void DesktopWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event)
        return;

    QFont menuFont(pingFangFontFamily());
    menuFont.setPixelSize(16);
    applyFontSmoothing(menuFont);
    LiquidGlassMenu menu(menuFont, this);
    QAction* addAction = menu.addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
    QAction* editorAction = m_cardKind == CardKind::Custom
        ? menu.addGlassAction(QStringLiteral("编辑此组件…"), LiquidGlassMenu::Icon::Edit) : nullptr;
    menu.addSeparator();
    QAction* cityAction = nullptr;
    QAction* countdownAction = nullptr;
    if (m_cardKind == CardKind::Countdown) {
        countdownAction = menu.addGlassAction(QStringLiteral("编辑倒数日…"), LiquidGlassMenu::Icon::Edit);
        menu.addSeparator();
    }
    if (m_cardKind == CardKind::Weather) {
        cityAction = menu.addGlassAction(QStringLiteral("搜索城市…"), LiquidGlassMenu::Icon::Search);
        menu.addSeparator();
    }
    QAction* removeAction = menu.addGlassAction(QStringLiteral("删除此组件"), LiquidGlassMenu::Icon::Remove);
    // 至少保留一个托盘管理者，避免进程运行却无法重新打开图库。
    removeAction->setEnabled(s_instances.size() > 1);
    QAction* selected = menu.exec(event->globalPos());
    if (selected == addAction) {
        showWidgetLibrary();
    } else if (editorAction && selected == editorAction) {
        openCustomWidgetEditor(m_custom.id);
    } else if (cityAction && selected == cityAction) {
        searchWeatherCity();
    } else if (countdownAction && selected == countdownAction) {
        editCountdown();
    } else if (selected == removeAction && removeAction->isEnabled()) {
        const bool wasPrimary = m_ownsTray;
        if (m_trayIcon)
            m_trayIcon->hide();

        // 保存前先从占位集合移除，避免已删除卡片残留配置或阻挡放置。
        s_instances.removeAll(this);
        if (wasPrimary && !s_instances.isEmpty()) {
            DesktopWidget* successor = s_instances.first();
            successor->m_ownsTray = true;
            successor->setupTrayIcon();
        }
        for (DesktopWidget* widget : s_instances) {
            if (widget && widget->m_ownsTray) {
                widget->updateTrayVisibilityLabel();
                widget->updateRearrangeLabel();
                break;
            }
        }
        saveAllConfigurations();
        hideGridPreview();
        hide();
        deleteLater();
    }
    event->accept();
}

void DesktopWidget::moveEvent(QMoveEvent* event)
{
    LiquidGlassWidget::moveEvent(event);
    if (!m_restoringPosition && windowDragging())
        showGridPreviewRect(nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this));
}
