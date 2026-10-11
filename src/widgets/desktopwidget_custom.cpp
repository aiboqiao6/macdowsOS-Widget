#include "widgets/desktopwidget.h"
#include "ui/htmlwidgetview.h"
#include "ui/widgeteditordialog.h"
#include "ui/widgetlibrarydialog.h"
#include <QPointer>
#include <QResizeEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <windows.h>

namespace {
QPointer<WidgetEditorDialog> editor;
}
void DesktopWidget::openCustomWidgetEditor(const QString& definitionId, QWidget* modalParent) {
    if (!editor) editor = new WidgetEditorDialog;
    if (!definitionId.isEmpty()) {
        bool found = false;
        for (const auto& definition : CustomWidgets::load()) if (definition.id == definitionId) {
            editor->selectDefinition(definition); found = true; break;
        }
        if (!found) for (const auto* widget : s_instances) if (widget->m_cardKind == CardKind::Custom && widget->m_custom.id == definitionId) {
            editor->selectDefinition(widget->m_custom); break;
        }
    }
    // 以设置窗口为原生拥有者，避免独立的应用模态窗口相互阻塞输入。
    const auto modality = modalParent ? Qt::WindowModal : Qt::NonModal;
    if (editor->parentWidget() != modalParent || editor->windowModality() != modality) {
        editor->hide();
        editor->setParent(modalParent, Qt::Dialog | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint);
        editor->setWindowModality(modality);
    }
    editor->show(); editor->raise(); editor->activateWindow();
}
void DesktopWidget::closeCustomWidgetEditor() { delete editor.data(); }
bool DesktopWidget::confirmCustomWidgetEditorClose() { return !editor || editor->close(); }
bool DesktopWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    // 浏览器内容允许键盘焦点，顶部拖动栏继续使用桌面卡片的非激活交互。
    if (m_cardKind == CardKind::Custom && !mouseThroughEnabled() && m_customView
        && eventType == "windows_generic_MSG" && message
        && static_cast<MSG*>(message)->message == WM_MOUSEACTIVATE
        && m_customView->geometry().contains(mapFromGlobal(QCursor::pos()))) {
        *result = MA_ACTIVATE;
        return true;
    }
    return LiquidGlassWidget::nativeEvent(eventType, message, result);
}
void DesktopWidget::resizeEvent(QResizeEvent* event) {
    LiquidGlassWidget::resizeEvent(event);
    if (m_customView) {
        const int margin = qMax(3, qRound(8 * m_widgetScale));
        const int header = qMax(20, qRound(34 * m_widgetScale));
        m_customView->setGeometry(margin, header, qMax(1, width() - margin * 2), qMax(1, height() - header - margin));
        m_customView->setContentScale(m_widgetScale);
    }
}
void DesktopWidget::applyCustomContent() {
    if (m_cardKind != CardKind::Custom || m_previewOnly) return;
    if (!m_customView) m_customView = new HtmlWidgetView(this);
    const int margin = qMax(3, qRound(8 * m_widgetScale));
    const int header = qMax(20, qRound(34 * m_widgetScale));
    m_customView->setGeometry(margin, header, qMax(1, width() - margin * 2), qMax(1, height() - header - margin));
    m_customView->setContentScale(m_widgetScale);
    m_customView->setDocument(CustomWidgets::sandboxedDocument(m_custom));
    m_customView->show();
    setToolTip(m_custom.name + QStringLiteral(" · 拖动顶部移动 · 双击顶部编辑"));
    update();
}
DesktopWidget* DesktopWidget::addCustomWidget(const CustomWidgets::Definition& definition, const QPoint& desktopPoint) {
    CustomWidgets::Definition checked;
    if (!CustomWidgets::fromJson(definition.toJson(), checked)) return nullptr;
    auto* widget = new DesktopWidget(nullptr, CardKind::Custom, false, {}, checked.variant);
    widget->m_custom = checked;
    widget->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!widget->placeAtDesktopPoint(desktopPoint)) { delete widget; return nullptr; }
    widget->applyCustomContent(); widget->show(); saveAllConfigurations(); return widget;
}
bool DesktopWidget::saveCustomDefinition(const CustomWidgets::Definition& definition, QString* error) {
    CustomWidgets::Definition checked;
    if (!CustomWidgets::fromJson(definition.toJson(), checked, error)) return false;
    struct Previous { DesktopWidget* widget; QRect geometry; int variant; };
    QList<Previous> previous;
    for (auto* widget : s_instances) if (widget->m_cardKind == CardKind::Custom && widget->m_custom.id == checked.id)
        previous.append({widget, widget->geometry(), widget->m_cardVariant});
    auto rollback = [&previous]() {
        for (const auto& entry : previous) {
            entry.widget->m_restoringPosition = true;
            entry.widget->m_cardVariant = entry.variant;
            entry.widget->setFixedSize(entry.geometry.size()); entry.widget->move(entry.geometry.topLeft());
            entry.widget->m_restoringPosition = false;
        }
    };
    for (const auto& entry : previous) {
        auto* widget = entry.widget;
        QScreen* screen = QGuiApplication::screenAt(widget->geometry().center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        const QRect area = screen ? screen->availableGeometry() : QRect();
        const QRect requested(widget->pos(), cardSize(CardKind::Custom, checked.variant, widget->m_widgetScale, area));
        const QRect target = area.contains(requested) && !overlapsVisibleWidget(requested, widget) ? requested
            : nearestGridRect(CardKind::Custom, checked.variant, widget->pos(), widget->m_widgetScale, widget);
        if (!target.isValid()) {
            rollback(); if (error) *error = QStringLiteral("桌面空位不足以容纳新尺寸。请释放空间或选择较小尺寸后保存。"); return false;
        }
        widget->m_restoringPosition = true; widget->m_cardVariant = checked.variant;
        widget->setFixedSize(target.size()); widget->move(target.topLeft()); widget->m_restoringPosition = false;
    }
    if (!CustomWidgets::save(checked, error)) { rollback(); return false; }
    for (const auto& entry : previous) { entry.widget->m_custom = checked; entry.widget->applyCustomContent(); }
    if (!previous.isEmpty()) saveAllConfigurations();
    WidgetLibraryDialog::refreshCustomWidgets();
    return true;
}
