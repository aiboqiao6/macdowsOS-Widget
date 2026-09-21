#pragma once

#include "qtglassflowscene.h"
#include "desktopcapture.h"

#include <QPoint>
#include <QImage>
#include <QElapsedTimer>
#include <QRect>
#include <QSize>
#include <QString>
#include <functional>

class QMouseEvent;
class QMoveEvent;
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
    ~LiquidGlassWidget() override;

    // Capture verified pixels below the target, then composite lower glass.
    // Covered external pixels use the last valid capture or wallpaper on a
    // cold start. Glass cards remain visible to system screenshot APIs.
    static QImage captureDesktopComposite(QScreen* screen, const QRect& area,
                                          LiquidGlassWidget* excluded = nullptr,
                                          qreal renderScale = 1.0);

    void setGlassMargins(int horizontal, int vertical = -1);
    void setGlassRadius(qreal radius);
    void configureGlass(float powerFactor = 3.8f,
                        float refractionPower = 1.32f,
                        float blurRadius = 4.5f,
                        int blurIterations = 3,
                        float noiseAmount = 0.012f);

    // Material switch. Blur-only keeps the live desktop sampling path but
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
    // Stationary desktop cards normally use the wallpaper-only path.  Live
    // backdrop mode samples the actual lower window stack as well, while the
    // capture compositor still removes this process' own glass surfaces to
    // prevent recursive glare.
    void setLiveBackdropEnabled(bool enabled);
    bool liveBackdropEnabled() const { return m_liveBackdropEnabled; }

    // A quality change also changes the required backdrop resolution.  The
    // base renderer invalidates its FBOs; this wrapper invalidates the desktop
    // crop as well so the new quality is visible immediately instead of after
    // the next idle wallpaper tick.
    void setRenderScale(float scale);

    // Shared polished edge used by any content widget. The actual blur,
    // clipping mask and rounded SDF remain in this base class; subclasses
    // only provide their own icons/text and call this helper for card rims.
    void paintGlassSurfaceFrame(QPainter& painter, const QRectF& rect,
                                qreal radius) const;

    void moveToDesktopCorner(int margin = 26);
    void captureDesktopBackdrop();
    // Hosts such as the widget gallery can reuse the exact QtGlassFlow
    // surface while supplying their own target-relative composite canvas.
    void setDesktopCaptureEnabled(bool enabled);
    bool desktopCaptureEnabled() const { return m_desktopCaptureEnabled; }
    void setDesktopLayerEnabled(bool enabled) { m_desktopLayerEnabled = enabled; }

protected:
    void setPanelWindow() { m_panelWindow = true; }
    static void preparePanelOpening();
    bool windowDragging() const { return m_dragging; }
    bool captureExclusionAvailable() const { return m_captureExcluded; }
    // Used by the capture detector and by focused regression coverage.
    void setSystemCaptureMode(bool active);
    void requestDesktopComposite(const QRect& area,
                                 std::function<void(QImage)> completed);
    virtual void windowDragStarted() {}
    virtual void windowDragFinished() {}
    // Concrete desktop widgets can keep a quiet wallpaper-only backdrop while
    // stationary, then opt into live lower-window compositing only during a
    // drag. Generic glass hosts (such as the library dialog and test surface)
    // retain their normal idle compositing behaviour.
    virtual bool wallpaperOnlyWhenIdle() const { return false; }
    // Desktop cards hide ordinary application HWNDs while being repositioned.
    // They can therefore crop the shared wallpaper and lower glass surfaces
    // directly at display cadence instead of waiting for desktop capture.
    virtual bool directBackdropDuringDrag() const { return false; }
    static bool desktopDragActive();
    // Millisecond cadence derived from the monitor containing this window.
    // Exposed to specialized glass hosts such as the widget gallery so every
    // live surface follows the same display-refresh policy.
    int activeDisplayInterval() const;
    bool event(QEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    friend class WidgetRegression;
    void updateGlassObjectGeometry();
    void updateWindowMask();
    bool rebuildWallpaperCanvas(QScreen* screen);
    QImage wallpaperBackdrop(const QRect& area, qreal scale);
    void applySystemBlurEffect();
    void updateMaterialParameters();
    void updateRefreshRate();
    bool hasOverlappingDesktopWidget() const;
    void refreshBackdropTopology();
    void updateCaptureExclusion(bool resetBackdrop = true);
    void updateSystemCaptureMode();
    bool captureCompositedBackdrop(QScreen* screen);
    void updateCompositeCrop();
    static void queueCompositeRefresh();
    static void refreshDragRendering();
    bool fullyOccluded() const;
    static void compositeGlassWindows(QImage& image, const QRect& area,
                                      const LiquidGlassWidget* excluded);
    void updateWindowLayer();
    void finishWindowDrag();

    // The reusable surface defaults to an edge-to-edge glass object.  A
    // subclass can opt into an inset explicitly with setGlassMargins().
    int m_glassMarginsX = 0;
    int m_glassMarginsY = 0;
    qreal m_glassRadius = 30.0;
    bool m_systemBlur = false;
    bool m_mouseThrough = false;
    bool m_lowPowerRefresh = false;
    bool m_liveBackdropEnabled = false;
    int m_blurStrength = 55;
    qreal m_glassOpacity = 1.0;
    bool m_desktopCaptureEnabled = true;
    bool m_desktopLayerEnabled = true;
    int m_glassObjectIndex = -1;
    bool m_dragging = false;
    // Only change the native Z order when the requested layer actually changes.
    bool m_windowLayerInitialized = false;
    bool m_panelWindow = false;
    bool m_captureExcluded = false;
    WId m_registeredGlassWindow = 0;
    bool m_systemCaptureActive = false;
    bool m_capturePending = false;
    quint64 m_captureGeneration = 0;
    DesktopCapture::Cache m_backdropCache;
    QImage m_compositeCanvas;
    QRect m_compositeArea;
    qint64 m_lastCropCanvasKey = 0;
    QRect m_lastCropGeometry;
    quint64 m_lastCropSceneRevision = 0;
    bool m_lastCropHadLayers = false;
    int m_unchangedCaptures = 0;
    QImage m_lastCompositedBackdrop;
    QElapsedTimer m_compositeCaptureClock;
    QRect m_lastCompositeGeometry;
    QPoint m_dragOffset;
    QPoint m_pressPosition;
    bool m_dragPending = false;
    QTimer* m_backdropTimer = nullptr;
    QTimer* m_captureMonitorTimer = nullptr;
    QImage m_wallpaperCanvas;
    QString m_wallpaperSignature;
    QString m_wallpaperScreenName;
    QSize m_wallpaperPixelSize;
    QElapsedTimer m_wallpaperRefreshClock;
};
