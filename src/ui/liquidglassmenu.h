#pragma once

#include "rendering/desktopcapture.h"
#include <QMenu>
#include <QPointF>
#include <QPointer>
#include <QTimer>

class QVariantAnimation;
class LiquidGlassMenuSurface;
class LiquidGlassWidget;
class MenuScrim;

// QMenu 管理输入、无障碍、布局和关闭；透明子窗口复用桌面玻璃渲染。
class LiquidGlassMenu final : public QMenu
{
public:
    enum class Icon { Add, Search, Remove, Edit };
    explicit LiquidGlassMenu(const QFont& font, QWidget* parent = nullptr);
    ~LiquidGlassMenu() override;
    QAction* addGlassAction(const QString& text, Icon icon);
    QAction* exec(const QPoint& position);
    void popup(const QPoint& position);
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;

private:
    friend class LiquidGlassMenuSurface;
    // 两种材质对应两套菜单外观：磨砂是 iPadOS 的整行分隔与整行高亮，
    // 液态玻璃是 macOS 的圆角高亮块与分组细线。
    struct MenuStyle {
        qreal radius = 16;
        int rowPadding = 8;        // 每行上下内边距
        int minWidth = 144;        // 菜单最小宽度：iPadOS 的菜单明显更宽
        int menuPadding = 7;       // 菜单外圈留白：iPadOS 的行底顶到边缘，取 0
        int separatorHeight = 9;   // 分组分隔的高度：磨砂样式只剩 1px，避免线与高亮之间露白
        bool rowDividers = false;  // 行与行之间的整条发丝线
        bool pillHighlight = true; // 悬停高亮是内缩的圆角块
        bool separatorLine = true; // 分组分隔符自己画一条短线
    };
    void paintContents(QPainter& painter);
    void refreshBackdrop();
    void releaseBackdrop();
    QPoint preparePopup(const QPoint& position);
    void createSurface();
    void applyMenuStyle();
    // 弹出动画：与设置页下拉菜单同一套弹簧，玻璃与内容一起从锚点角弹到位。
    void startOpenAnimation();
    void tickSpring();
    void applyOpenProgress(qreal progress);
    LiquidGlassMenuSurface* m_surface = nullptr;
    // 回收动画：菜单走 hide 或直接被析构两条路径都要触发，只播一次。
    void spawnClosingGhost();
    // 没有回收动画可播时，蒙版与宿主图层要在这里收尾。
    void dropOwnerElevation();
    MenuScrim* m_scrim = nullptr;
    // 压暗层铺满全屏后不再挖洞：打开期间把宿主卡片与菜单抬到蒙版上面，
    // 关闭动画结束时再把卡片放回桌面层。
    void applyMenuLayering(bool finalPass = false);
    QPointer<LiquidGlassWidget> m_ownerGlass;
    WId m_ownerWindow = 0;
    quint64 m_ownerElevation = 0;
    bool m_ghostSpawned = false;
    QTimer* m_openTicker = nullptr;
    QPointF m_openAnchor{0.0, 0.0};   // 菜单从离组件最近的那只角长出来。
    qreal m_progress = 1.0;
    qreal m_velocity = 0.0;
    MenuStyle m_style;
    QTimer m_captureTimer;
    DesktopCapture::Cache m_cache;
    QImage m_fallback;
    QImage m_lastBackdrop;
    WId m_registeredWindow = 0;
    quint64 m_generation = 0;
    bool m_capturePending = false;
    bool m_liveBackdrop = false;
    float m_renderScale = 1.0f;
};
