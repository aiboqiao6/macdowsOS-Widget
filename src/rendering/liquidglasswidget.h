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

// 可复用桌面玻璃窗口，负责透明表面、背景采样、层级与拖动；具体组件实现 paintOverlay。
class LiquidGlassWidget : public QtGlassFlowScene
{
public:
    explicit LiquidGlassWidget(QWidget* parent = nullptr);
    ~LiquidGlassWidget() override;

    // 捕获目标下方已验证像素并合成下层玻璃；冷启动缺失区域使用壁纸，组件仍可被系统截图。
    static QImage captureDesktopComposite(QScreen* screen, const QRect& area,
                                          LiquidGlassWidget* excluded = nullptr,
                                          qreal renderScale = 1.0);
    // 弹出面板复用下层玻璃合成；输入图像必须只包含验证过的外部像素。
    static void compositeDesktopGlass(QImage& image, const QRect& area, WId target = 0);
    // Reuse a desktop widget's wallpaper cache when a panel has no verified capture pixels yet.
    static QImage desktopWallpaper(const QRect& area, qreal scale);

    void setGlassMargins(int horizontal, int vertical = -1);
    void setGlassRadius(qreal radius);
    // 玻璃圆角：右键菜单的压暗层要按卡片真实的圆角挖洞，不能用估算值。
    qreal glassRadius() const { return m_glassRadius; }
    // 启用鼠标穿透；独立的托盘与设置窗口继续接收输入。
    void setMouseThroughEnabled(bool enabled);
    bool mouseThroughEnabled() const { return m_mouseThrough; }
    void setLowPowerRefreshEnabled(bool enabled);
    bool lowPowerRefreshEnabled() const { return m_lowPowerRefresh; }
    // 关闭实时背景时使用壁纸；开启后采样下层窗口，并剔除本进程玻璃以防反馈。
    void setLiveBackdropEnabled(bool enabled, bool persist = true);
    bool liveBackdropEnabled() const { return m_liveBackdropEnabled; }

    // 画质变化时同时失效 FBO 与桌面裁剪缓存，使新分辨率立即生效。
    void setRenderScale(float scale);

    void moveToDesktopCorner(int margin = 26);
    void captureDesktopBackdrop();
    // 允许图库等宿主自行提供目标区域的背景合成画布。
    void setDesktopCaptureEnabled(bool enabled);
    bool desktopCaptureEnabled() const { return m_desktopCaptureEnabled; }
    void setDesktopLayerEnabled(bool enabled) { m_desktopLayerEnabled = enabled; }

    // 右键菜单的压暗层铺满全屏，不再挖洞；打开期间把这张卡片的窗口临时抬到蒙版之上，
    // 关闭动画结束后再放回桌面层。序号用来区分先后两次打开，后开的那次不会被旧的收回压回去。
    quint64 elevateForMenu(WId aboveWindow);
    void dropMenuElevation(quint64 serial);

protected:
    void setPanelWindow() { m_panelWindow = true; }
    static void preparePanelOpening();
    bool windowDragging() const { return m_dragging; }
    bool captureExclusionAvailable() const { return m_captureExcluded; }
    // 供截图检测与回归测试切换采样状态。
    void setSystemCaptureMode(bool active);
    void requestDesktopComposite(const QRect& area,
                                 std::function<void(QImage)> completed);
    virtual void windowDragStarted() {}
    virtual void windowDragFinished() {}
    // 组件可选择静止时仅采样壁纸，拖动时再合成实时下层画面。
    virtual bool wallpaperOnlyWhenIdle() const { return false; }
    // 拖动卡片期间普通应用窗口隐藏，可直接裁剪壁纸与下层玻璃以保持刷新速度。
    virtual bool directBackdropDuringDrag() const { return false; }
    static bool desktopDragActive();
    // 按窗口所在显示器计算刷新间隔，供所有玻璃宿主复用。
    int activeDisplayInterval() const;
    bool event(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
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
    // 壁纸换代后丢弃所有基于旧壁纸的缓存，并让暂停的卡片立即重绘。
    void invalidateWallpaperBackdrop();
    QImage wallpaperBackdrop(const QRect& area, qreal scale);
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
                                      const LiquidGlassWidget* excluded, WId target = 0);
    void updateWindowLayer();
    void finishWindowDrag();

    // 默认玻璃覆盖整个窗口，可通过 setGlassMargins 显式设置内边距。
    int m_glassMarginsX = 0;
    int m_glassMarginsY = 0;
    qreal m_glassRadius = 30.0;
    bool m_mouseThrough = false;
    bool m_lowPowerRefresh = false;
    bool m_liveBackdropEnabled = false;
    bool m_desktopCaptureEnabled = true;
    bool m_desktopLayerEnabled = true;
    int m_glassObjectIndex = -1;
    bool m_dragging = false;
    // 仅在目标层级变化时调整原生窗口次序。
    bool m_windowLayerInitialized = false;
    // 菜单打开期间压在 updateWindowLayer 之上，避免它把卡片又沉回蒙版下面。
    bool m_menuElevated = false;
    quint64 m_menuElevationSerial = 0;
    bool m_panelWindow = false;
    bool m_captureExcluded = false;
    WId m_registeredGlassWindow = 0;
    bool m_systemCaptureActive = false;
    int m_capturesInFlight = 0;
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
    // 已应用到本窗口的壁纸代际；与进程共享代际不同即表示壁纸换过。
    quint64 m_wallpaperEpoch = 0;
};
