#pragma once

#include "liquidglasswidget.h"

#include <QList>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QTimer>

class QFrame;
class QShowEvent;
class QPainter;
class QPropertyAnimation;
class QMouseEvent;

// A small, frameless widget gallery. Tiles start a real Qt drag operation;
// releasing outside the gallery emits widgetDropped with the desktop point.
class WidgetLibraryDialog final : public LiquidGlassWidget
{
    Q_OBJECT
public:
    explicit WidgetLibraryDialog(QWidget* parent = nullptr);

    // Capture the already-composited desktop while this window is hidden.
    // The shared QtGlassFlow shader then blurs/refractions this image, so
    // application windows are included instead of sampling wallpaper only.
    void prepareBackdrop();

    void notifyWidgetDropped(int kind, const QPoint& globalPos);
    void notifyWidgetDragPreview(int kind, const QPoint& globalPos, bool visible);

signals:
    void widgetDropped(int kind, const QPoint& globalPos);
    void widgetDragPreview(int kind, const QPoint& globalPos, bool visible);

private:
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintOverlay(QPainter& painter) override;
    void closeGallery();
    void refreshBackdrop();
    void enableCaptureExclusion();
    QList<QFrame*> m_tiles;
    QPropertyAnimation* m_slideAnimation = nullptr;
    QTimer m_renderTimer;
    QTimer m_liveBackdropTimer;
    QRect m_backdropRect;
    QImage m_lastBackdropImage;
    bool m_captureExcluded = false;
    bool m_closing = false;
};
