#pragma once

#include "qtglassflowscene.h"

#include <QPoint>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

class QMouseEvent;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QHideEvent;
class QTimer;

// Reusable desktop liquid-glass surface.
//
// This class intentionally contains no widget-specific UI. It owns the
// QtGlassFlow SDF/FBO pipeline, transparent window setup, rounded mask,
// desktop placement, backdrop sampling, and window dragging. Concrete
// widgets only need to override paintOverlay() and add their own content.
class LiquidGlassWidget : public QtGlassFlowScene
{
public:
    explicit LiquidGlassWidget(QWidget* parent = nullptr);

    void setGlassMargins(int horizontal, int vertical = -1);
    void setGlassRadius(qreal radius);
    void configureGlass(float powerFactor = 3.8f,
                        float refractionPower = 1.32f,
                        float blurRadius = 4.5f,
                        int blurIterations = 3,
                        float noiseAmount = 0.012f);

    // Material switch. Blur-only keeps the wallpaper-only rendering path but
    // disables refraction, avoiding DWM/OpenGL compositor artifacts.
    void setSystemBlurEnabled(bool enabled);
    bool systemBlurEnabled() const { return m_systemBlur; }
    void setMaterialBlurStrength(int percent);
    int materialBlurStrength() const { return m_blurStrength; }
    void setGlassOpacity(qreal opacity);
    qreal glassOpacity() const { return m_glassOpacity; }

    // Let the widget become a purely visual desktop surface. The tray and
    // settings dialogs remain usable because they are separate windows.
    void setMouseThroughEnabled(bool enabled);
    bool mouseThroughEnabled() const { return m_mouseThrough; }
    void setLowPowerRefreshEnabled(bool enabled);
    bool lowPowerRefreshEnabled() const { return m_lowPowerRefresh; }

    // Shared polished edge used by any content widget. The actual blur,
    // clipping mask and rounded SDF remain in this base class; subclasses
    // only provide their own icons/text and call this helper for card rims.
    void paintGlassSurfaceFrame(QPainter& painter, const QRectF& rect,
                                qreal radius) const;

    void moveToDesktopCorner(int margin = 26);
    void captureDesktopBackdrop();
    // Hosts such as the widget gallery can reuse the exact QtGlassFlow
    // surface while supplying their own composite screenshot (including
    // foreground windows) instead of the wallpaper-only desktop sampler.
    void setDesktopCaptureEnabled(bool enabled);
    bool desktopCaptureEnabled() const { return m_desktopCaptureEnabled; }
    void setDesktopLayerEnabled(bool enabled) { m_desktopLayerEnabled = enabled; }

protected:
    bool windowDragging() const { return m_dragging; }
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void updateGlassObjectGeometry();
    void updateWindowMask();
    bool rebuildWallpaperCanvas(QScreen* screen);
    void applySystemBlurEffect();
    void updateMaterialParameters();
    void updateRefreshRate();

    // The reusable surface defaults to an edge-to-edge glass object.  A
    // subclass can opt into an inset explicitly with setGlassMargins().
    int m_glassMarginsX = 0;
    int m_glassMarginsY = 0;
    qreal m_glassRadius = 30.0;
    bool m_systemBlur = false;
    bool m_mouseThrough = false;
    bool m_lowPowerRefresh = false;
    int m_blurStrength = 55;
    qreal m_glassOpacity = 1.0;
    bool m_desktopCaptureEnabled = true;
    bool m_desktopLayerEnabled = true;
    int m_glassObjectIndex = -1;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QTimer* m_backdropTimer = nullptr;
    QImage m_wallpaperCanvas;
    QString m_wallpaperSignature;
    QString m_wallpaperScreenName;
    QSize m_wallpaperPixelSize;
    QRect m_lastBackdropGeometry;
    int m_wallpaperPollTick = 0;
};
