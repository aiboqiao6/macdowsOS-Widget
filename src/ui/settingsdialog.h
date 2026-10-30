#pragma once

#include "rendering/desktopcapture.h"
#include <QDialog>
#include <QPointer>
#include <QVector>

class QVBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class UpdateChecker;
class SettingsFrameClock;

class SettingsDialog final : public QDialog
{
public:
    enum Page { General, Appearance, Material, Weather, Widgets, About };
    explicit SettingsDialog(bool darkMode);
    ~SettingsDialog() override;
    QVBoxLayout* addGroup(Page page, const QString& title = {});
    void addRow(QVBoxLayout* group, const QString& label, QWidget* control,
                const QString& description = {});
    void setDarkAppearance(bool dark);
    void setInterfaceScale(qreal scale);
    void selectPage(int index);
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
private:
    void refreshBackdrop();
    void presentBackdrop(DesktopCapture::Frame frame);
    void refreshLayout();
    void updateInterfaceScale();
    void refreshNavigationInk();
    void showPage(int index);
    void navigateHistory(int offset);
    void filterPages(const QString& query);
    void buildAboutPage();
    void buildUpdateSection();
    void showUpdateDialog(UpdateChecker* checker);
    QWidget* m_blur;
    QWidget* m_sidebar;
    QStackedWidget* m_stack;
    QLabel* m_title;
    QPushButton* m_back;
    QPushButton* m_forward;
    QLabel* m_noResults;
    QLineEdit* m_search;
    QVector<QVBoxLayout*> m_pages;
    QVector<QPushButton*> m_navigation;
    QVector<QPointer<QWidget>> m_rowControls;
    QVector<int> m_pageHistory;
    int m_historyPosition = -1;
    bool m_dark = false;
    bool m_capturePending = false;
    bool m_dragging = false;
    bool m_layoutDirty = true;
    bool m_updatingScale = false;
    qreal m_interfaceScale = 1;
    qreal m_effectiveScale = 1;
    QFont m_baseFont;
    QSizeF m_unscaledWindowSize;
    QMetaObject::Connection m_screenConnection;
    QFont m_layoutFont;
    qreal m_layoutDpi = 0, m_layoutDpr = 0;
    int m_layoutWidth = -1;
    int m_controlMaximum = -1;
    QPoint m_dragOffset;
    quint64 m_generation = 0;
    WId m_registeredWindow = 0;
    SettingsFrameClock* m_frameClock = nullptr;
    QImage m_lastBackdrop;
    QElapsedTimer m_captureCadence;
};
