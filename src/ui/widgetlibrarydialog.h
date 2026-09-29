#pragma once

#include "rendering/liquidglasswidget.h"

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

// A frameless gallery. Mouse capture keeps tile drags local so Escape and
// interrupted drags cannot be mistaken for an unsupported desktop drop.
class WidgetLibraryDialog final : public LiquidGlassWidget
{
    Q_OBJECT
public:
    explicit WidgetLibraryDialog(QWidget* parent = nullptr);
    static void refreshFontRendering();
    void setInterfaceScale(qreal scale);

    // Capture the already-composited desktop while this window is hidden.
    // The shared QtGlassFlow shader then blurs/refractions this image, so
    // application windows are included instead of sampling wallpaper only.
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
    int windowScaled(int value) const;
    int tileColumnCount() const;
    void randomizeRecommendations();
    QList<QFrame*> m_tiles;
    QList<QPushButton*> m_navButtons;
    QGridLayout* m_tileLayout = nullptr;
    QVBoxLayout* m_sideLayout = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QHBoxLayout* m_footerLayout = nullptr;
    QFrame* m_sidebar = nullptr;
    QFrame* m_footer = nullptr;
    QPushButton* m_doneButton = nullptr;
    QLabel* m_sectionTitle = nullptr;
    QLineEdit* m_search = nullptr;
    qreal m_uiScale = 1.0;
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
    bool m_closing = false;
};
