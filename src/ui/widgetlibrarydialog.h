#pragma once

#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"

#include <QList>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QTimer>

class QFrame;
class QEvent;
class QShowEvent;
class QPainter;
class QPropertyAnimation;
class QMouseEvent;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class QScrollArea;
class QPushButton;
class QLabel;
class QLineEdit;

// 无边框图库通过鼠标捕获管理拖动，退出或中断时不会误判为桌面放置。
class WidgetLibraryDialog final : public LiquidGlassWidget
{
    Q_OBJECT
public:
    explicit WidgetLibraryDialog(QWidget* parent = nullptr);
    ~WidgetLibraryDialog() override;
    static void refreshPreviews();
    void updateScreenLayout();

    // 图库隐藏时捕获完整桌面，再由共享玻璃管线处理模糊和折射。
    void prepareBackdrop();

    void notifyWidgetDropped(int kind, const QPoint& globalPos);
    void notifyWidgetDragStarted(int kind);
    void notifyWidgetDragFinished(int kind);
    void notifyWidgetDragPreview(int kind, const QPoint& globalPos, bool visible);

signals:
    void widgetDropped(int kind, const QPoint& globalPos);
    void widgetDragStarted(int kind);
    void widgetDragFinished(int kind);
    void widgetDragPreview(int kind, const QPoint& globalPos, bool visible);

private:
    friend class WidgetRegression;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintOverlay(QPainter& painter) override;
    void closeGallery();
    void refreshBackdrop();
    void updateBackdropFrame();
    void selectCategory(int category);
    void applyTileFilter();
    int scaled(int value) const;
    static qreal fittedScaleForArea(const QRect& area, qreal scale);
    int tileColumnCount() const;
    void resetRecommendations();
    QList<QFrame*> m_tiles;
    QList<QPushButton*> m_navButtons;
    QList<QLabel*> m_categoryTitles;
    QGridLayout* m_tileLayout = nullptr;
    QVBoxLayout* m_sideLayout = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QHBoxLayout* m_footerLayout = nullptr;
    QFrame* m_sidebar = nullptr;
    QFrame* m_footer = nullptr;
    QPushButton* m_doneButton = nullptr;
    QLabel* m_sectionTitle = nullptr;
    QLineEdit* m_search = nullptr;
    qreal m_interfaceScale = 1.0;
    QSize m_preferredSize;
    bool m_scaleApplied = false;
    int m_tileColumns = 0;
    int m_selectedCategory = -1;
    QList<int> m_recommendedTiles;
    QPropertyAnimation* m_slideAnimation = nullptr;
    QTimer m_renderTimer;
    QTimer m_liveBackdropTimer;
    QRect m_backdropRect;
    QRect m_backdropCaptureRect;
    QRect m_lastBackdropGeometry;
    QImage m_backdropCanvas;
    QImage m_lastBackdropImage;
    int m_liveCapturesInFlight = 0;
    quint64 m_liveBackdropFrames = 0;
    quint64 m_liveBackdropSamples = 0;
    quint64 m_backdropGeneration = 0;
    int m_unchangedBackdropSamples = 0;
    NativeWindows::HiddenWindows m_hiddenApplicationWindows;
    bool m_desktopPrepared = false;
    bool m_initialBackdropPending = false;
    bool m_closing = false;
};
