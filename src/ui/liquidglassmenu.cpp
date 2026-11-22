#include "liquidglassmenu.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "rendering/renderdiagnostics.h"
#include "rendering/responsivelayout.h"
#include "ui/settingsmotion.h"
#include "widgets/cardink.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QVector>
#include "app/appsettings.h"
#include <QShowEvent>
#include <cmath>
#include <QtMath>

namespace {
constexpr qreal kRadius = 16;
// 弹簧回弹时面板会长过最终尺寸；窗口比面板每侧多留这么多像素，
// 回弹就有地方长出去，不会被窗口边界裁掉圆角。
constexpr int kPanelInset = 10;
constexpr auto kIconProperty = "liquidGlassMenuIcon";
// 与设置页下拉菜单同一套弹簧：progress 峰值 1.0659，缩放峰值 0.60 + 0.40 × 1.0659 ≈ 1.026。
// 静止时缩放保持 1.0（面板填满窗口），只有回弹瞬间短暂超出，由窗口边界裁掉。

void drawIcon(QPainter& painter, const QRectF& box, LiquidGlassMenu::Icon icon)
{
    painter.save();
    painter.translate(box.topLeft());
    painter.scale(box.width() / 18.0, box.height() / 18.0);
    QPen pen = painter.pen();
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    switch (icon) {
    case LiquidGlassMenu::Icon::Add:
        painter.drawRoundedRect(QRectF(2, 2, 14, 14), 4, 4);
        painter.drawLine(QPointF(5.5, 9), QPointF(12.5, 9));
        painter.drawLine(QPointF(9, 5.5), QPointF(9, 12.5));
        break;
    case LiquidGlassMenu::Icon::Search:
        painter.drawEllipse(QRectF(2.5, 2.5, 9, 9));
        painter.drawLine(QPointF(10.5, 10.5), QPointF(15.5, 15.5));
        break;
    case LiquidGlassMenu::Icon::Remove:
        painter.drawLine(QPointF(2.5, 4.5), QPointF(15.5, 4.5));
        painter.drawRoundedRect(QRectF(6, 1.5, 6, 3), 1, 1);
        painter.drawRoundedRect(QRectF(4, 4.5, 10, 12), 2, 2);
        painter.drawLine(QPointF(7, 8), QPointF(7, 13));
        painter.drawLine(QPointF(11, 8), QPointF(11, 13));
        break;
    case LiquidGlassMenu::Icon::Edit:
        painter.drawPolygon(QPolygonF{QPointF(3, 11), QPointF(11, 3), QPointF(15, 7), QPointF(7, 15), QPointF(2, 16)});
        painter.drawLine(QPointF(9, 5), QPointF(13, 9));
        break;
    }
    painter.restore();
}
}

// 全屏压暗层：直接铺满整块屏幕，不再按元素挖洞。
// 需要保持原样的卡片与菜单靠原生窗口层级抬到蒙版之上（见 elevateForMenu），
// 这样蒙版就是一张规整的半透明黑，边缘不会出现 alpha 清除带来的锯齿和接缝。
class MenuScrim final : public QWidget
{
public:
    explicit MenuScrim(const QRect& screenGeometry)
        : QWidget(nullptr)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(screenGeometry);
    }
    void setLevel(qreal level)
    {
        const qreal next = qBound<qreal>(0.0, level, 1.0);
        if (qFuzzyCompare(m_level + 1.0, next + 1.0))
            return;
        m_level = next;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_level <= 0.01)
            return;
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, qRound(92 * m_level)));
    }
private:
    qreal m_level = 0.0;
};

// 回收动画：菜单关闭时用最后一帧做一块幽灵窗口，淡出并轻微回缩，
// 同时把压暗层一起淡掉。QMenu 的关闭路径不能延迟，所以用快照代替真实窗口。
class MenuClosingGhost final : public QWidget
{
public:
    MenuClosingGhost(const QImage& frame, const QRect& frameGeometry,
                     qreal anchorX, qreal anchorY, MenuScrim* scrim,
                     LiquidGlassWidget* owner, quint64 elevation, WId ownerWindow)
        : QWidget(nullptr), m_frame(frame), m_anchorX(anchorX), m_anchorY(anchorY),
          m_scrim(scrim), m_owner(owner), m_elevation(elevation), m_ownerWindow(ownerWindow)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(frameGeometry);
        // 与设置页下拉菜单的收盘用同一条弹簧（stiffness 1200 / damping 70，临界阻尼），
        // 而不是固定时长的缓动曲线，两处的收起手感才一致。
        m_ticker = new QTimer(this);
        m_ticker->setTimerType(Qt::PreciseTimer);
        m_ticker->setInterval(8);
        connect(m_ticker, &QTimer::timeout, this, [this]() { tick(); });
        m_clock.start();
    }
    // 必须先把窗口显示出来并画完第一帧，弹簧才开始跑：之前只在构造函数里启动定时器，
    // 窗口从头到尾没有 show()，弹簧就在一张看不见的窗口上跑完了，所以永远看不到回收动画。
    void startFade()
    {
        show();
        // 压暗层在最下面、宿主卡片压在它上面，回收面板必须再抬一次才盖在卡片上。
        NativeWindows::placeDragging(winId());
        repaint();
        m_ticker->start();
    }
    ~MenuClosingGhost() override { releaseScrim(); }
    bool scrimAttached() const { return m_scrim != nullptr; }
    // 蒙版与宿主层级一起收尾：先撤蒙版，再把卡片放回桌面层。
    void releaseScrim()
    {
        if (m_scrim) {
            delete m_scrim;
            m_scrim = nullptr;
        }
        if (m_owner) {
            m_owner->dropMenuElevation(m_elevation);
            m_owner = nullptr;
        }
    }
protected:
    void tick()
    {
        constexpr qreal dt = 0.008;
        const qreal stiffness = SettingsAppearance::Motion::popupStiffnessOut;
        const qreal damping = SettingsAppearance::Motion::popupDampingOut;
        m_velocity += (-stiffness * m_level - damping * m_velocity) * dt;
        m_level += m_velocity * dt;
        if (m_level <= 0.012) {
            m_level = 0.0;
            m_ticker->stop();
            if (m_scrim)
                m_scrim->setLevel(0.0);
            update();
            releaseScrim();
            close();
            deleteLater();
            RenderDiagnostics::write(QStringLiteral("menu.ghost"),
                QStringLiteral("done frames=%1 elapsed=%2ms")
                    .arg(m_frames).arg(m_clock.elapsed()));
            return;
        }
        // 压暗层比菜单更早退场（约 2.4 倍速）：菜单还在淡出时，桌面已经恢复，
        // 否则面板收回后还会残留一小块暗色痕迹。
        if (m_scrim)
            m_scrim->setLevel(qMin<qreal>(1.0, m_level * 2.4));
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        if (m_frame.isNull())
            return;
        QPainter painter(this);
        painter.setOpacity(m_level);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // 与展开同一套映射：收起时缩回起始比例。
        const qreal scale = SettingsAppearance::Motion::popupScaleFrom
                          + (1.0 - SettingsAppearance::Motion::popupScaleFrom) * m_level;
        painter.translate((width() - width() * scale) * m_anchorX,
                          (height() - height() * scale) * m_anchorY);
        painter.scale(scale, scale);
        // 快照是设备像素（菜单 417x195 @ DPR1.5，逻辑尺寸 278x130），
        // 按 1:1 画会超出窗口被裁掉一圈，必须铺满整个逻辑矩形。
        painter.drawImage(QRectF(0, 0, width(), height()), m_frame);
        ++m_frames;
    }
private:
    QImage m_frame;
    qreal m_anchorX = 0.0;
    qreal m_anchorY = 0.0;
    qreal m_level = 1.0;
    qreal m_velocity = 0.0;
    QTimer* m_ticker = nullptr;
    MenuScrim* m_scrim = nullptr;
    QPointer<LiquidGlassWidget> m_owner;
    quint64 m_elevation = 0;
    WId m_ownerWindow = 0;
    QElapsedTimer m_clock;
    int m_frames = 0;
};

class LiquidGlassMenuSurface final : public QtGlassFlowScene
{
public:
    explicit LiquidGlassMenuSurface(LiquidGlassMenu* menu)
        : QtGlassFlowScene(menu), m_menu(menu)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setAnimationEnabled(false);
        // 菜单与卡片一样跟随外观模式：亮色白磨砂配深色墨，暗色深磨砂配浅色墨。
        setLightSurface(true);
        m_object = addGlassObject({}, QSizeF(menu->size()));
        setGlassObjectCornerRadius(m_object, float(kRadius));
    }
    void fit(const QRect& bounds)
    {
        setGeometry(bounds);
        m_bounds = bounds.size();
        updateOpenGeometry();
    }
    void setRadius(qreal radius)
    {
        setGlassObjectCornerRadius(m_object, float(radius));
        update();
    }
    void setOpenAnchor(const QPointF& anchor)
    {
        m_openAnchor = anchor;
        updateOpenGeometry();
    }
    // 弹出动画进度：玻璃与内容按同一比例从锚点角长到最终大小。
    void setOpenProgress(qreal progress)
    {
        if (qFuzzyCompare(m_progress, progress))
            return;
        m_progress = progress;
        updateOpenGeometry();
        update();
    }
    // 整体淡入直接作用在玻璃透明度上，不用窗口透明度：后者每帧触发 DWM 重新合成，
    // 与子窗口的 GL 合成叠加时会掉帧，透明度也会跳变。
    void setSurfaceOpacity(qreal opacity)
    {
        const qreal next = qBound<qreal>(0.0, opacity, 1.0);
        if (qFuzzyCompare(m_surfaceOpacity, next))
            return;
        m_surfaceOpacity = next;
        setGlassOpacity(float(next));
        update();
    }
protected:
    void paintOverlay(QPainter& painter) override
    {
        // 墨色与表面一起翻转，避免亮色磨砂上出现白色文字。
        CardInk::setDark(darkModeEnabled());
        painter.save();
        painter.setOpacity(m_surfaceOpacity);
        painter.translate(panelRect().topLeft() + openOffset());
        painter.scale(openScale(), openScale());
        // 内容严格裁在面板的圆角里：窗口四周的 10px 是留给回弹的透明留白，
        // 行底、高亮和分割线都不能画到面板之外。
        QPainterPath panelPath;
        panelPath.addRoundedRect(QRectF(QPointF(0, 0), panelRect().size()), kRadius, kRadius);
        painter.setClipPath(panelPath);
        m_menu->paintContents(painter);
        painter.restore();
    }
private:
    // 面板就是窗口去掉四周 kPanelInset 的部分；行底顶到面板边缘（由菜单圆角裁形）。
    QRectF panelRect() const
    {
        return QRectF(QPointF(kPanelInset, kPanelInset),
                      QSizeF(qMax(1, m_bounds.width() - kPanelInset * 2),
                             qMax(1, m_bounds.height() - kPanelInset * 2)));
    }
    qreal openScale() const
    {
        // 与设置里的下拉菜单同一套弹簧：从 60% 长起，带初速，越过 1.0 后回弹一次。
        // 窗口比面板每侧大 kPanelInset，回弹的空间由留白吸收；这里再按面板宽度
        // 夹一次上限，保证再宽的菜单也不会长出留白。
        const qreal from = SettingsAppearance::Motion::popupScaleFrom;
        const QRectF panel = panelRect();
        const qreal limit = 1.0 + kPanelInset / qMax<qreal>(1.0, panel.width());
        return qMin(limit, from + (1.0 - from) * m_progress);
    }
    QPointF openOffset() const
    {
        const QRectF panel = panelRect();
        const qreal scale = openScale();
        return QPointF((panel.width() - panel.width() * scale) * m_openAnchor.x(),
                       (panel.height() - panel.height() * scale) * m_openAnchor.y());
    }
    void updateOpenGeometry()
    {
        const QRectF panel = panelRect();
        const qreal scale = openScale();
        setGlassObjectGeometry(m_object, panel.topLeft() + openOffset(),
                               QSizeF(panel.width() * scale, panel.height() * scale));
    }
    LiquidGlassMenu* m_menu;
    int m_object = -1;
    QSize m_bounds;
    QPointF m_openAnchor{0.0, 0.0};
    qreal m_progress = 1.0;
    qreal m_surfaceOpacity = 1.0;
};

LiquidGlassMenu::LiquidGlassMenu(const QFont& textFont, QWidget* parent)
    : QMenu(parent)
{
    setObjectName(QStringLiteral("liquidGlassContextMenu"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFont(textFont);
    // QMenu 只负责布局和命中区域，所有像素由透明玻璃子窗口绘制。
    applyMenuStyle();
    AppSettings settings;
    m_renderScale = qBound(.5f, settings.value(QStringLiteral("performance/renderScale"), 0.60).toFloat(), 1.5f);
    m_liveBackdrop = settings.value(QStringLiteral("appearance/liveBackdrop"), true).toBool();
    m_captureTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_captureTimer, &QTimer::timeout, this, &LiquidGlassMenu::refreshBackdrop);
    connect(this, &QMenu::hovered, this, [this]() { if (m_surface) m_surface->update(); });
}

void LiquidGlassMenu::createSurface()
{
    if (m_surface)
        return;
    m_surface = new LiquidGlassMenuSurface(this);
    m_surface->setRenderScale(m_renderScale);
    m_surface->setRadius(m_style.radius);
}

void LiquidGlassMenu::applyMenuStyle()
{
    AppSettings settings;
    const LiquidGlassStyle::Style material = LiquidGlassStyle::styleFromIndex(
        settings.value(QStringLiteral("material/style"),
                       LiquidGlassStyle::styleIndex(LiquidGlassStyle::kDefaultStyle)).toInt());
    MenuStyle style;
    if (material == LiquidGlassStyle::Style::Ios18Blur) {
        // 磨砂玻璃：iPadOS 的行底顶到菜单边缘，没有内缩，首尾行由菜单圆角裁出形状；
        // 行更紧（扁长），行间一条发丝线，分组只用间距。
        style.radius = 16;
        style.rowPadding = 7;
        style.minWidth = 208;
        style.menuPadding = kPanelInset;
        style.separatorHeight = 1;
        style.rowDividers = true;
        style.pillHighlight = false;
        style.separatorLine = false;
    } else {
        // 液态玻璃：macOS 的内缩圆角高亮块与分组短线，行更松、菜单更窄。
        style.radius = 18;
        style.rowPadding = 8;
        style.minWidth = 144;
        style.menuPadding = kPanelInset;
        style.separatorHeight = 9;
        style.rowDividers = false;
        style.pillHighlight = true;
        style.separatorLine = true;
    }
    m_style = style;
    setStyleSheet(QStringLiteral(
        "QMenu { background: transparent; border: none; padding: %3px; }"
        "QMenu::item { padding: %1px 14px %1px 36px; min-width: %2px; }"
        "QMenu::separator { height: %4px; margin: 0px 12px; }")
        .arg(style.rowPadding).arg(style.minWidth).arg(style.menuPadding)
        .arg(style.separatorHeight));
}

LiquidGlassMenu::~LiquidGlassMenu()
{
    // 右键菜单是临时对象，多数情况下 exec() 返回后直接析构、不经过 hideEvent，
    // 所以回收动画必须在这里也触发一次。
    spawnClosingGhost();
    // 没有回收动画可播（例如快照为空）时，蒙版与卡片层级要在这里收尾，
    // 否则压暗层会一直盖在桌面上、卡片也回不到桌面层。
    dropOwnerElevation();
    releaseBackdrop();
    delete m_surface;
    m_surface = nullptr;
}

QAction* LiquidGlassMenu::addGlassAction(const QString& text, Icon icon)
{
    QAction* action = addAction(text);
    action->setProperty(kIconProperty, int(icon));
    return action;
}

void LiquidGlassMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
    // 材质可能在两次弹出之间被切换，外观与圆角都要重新取。
    applyMenuStyle();
    createSurface();
    m_surface->setRadius(m_style.radius);
    m_surface->fit(rect());
    m_surface->setOpenAnchor(m_openAnchor);
    // 每次弹出都按当前材质重建表面：右键菜单跟随“液态玻璃 / 磨砂玻璃”。
    m_surface->reloadMaterialFromSettings();
    // 右键菜单是浮层，不画外描边（材质预设里的 hairline 在这里覆盖为 0）。
    m_surface->setHairlineAmount(0.0f);
    // 先缩到起始比例再显示，避免完整尺寸的一帧闪出。
    applyOpenProgress(0.0);
    // 全屏压暗层：整屏一层黑，不挖洞。要露出来的卡片与菜单靠原生窗口层级抬到蒙版上面，
    // 这样蒙版的边界永远是屏边，不会再出现圆角洞口的锯齿和接缝。
    m_ownerWindow = parentWidget() ? parentWidget()->winId() : 0;
    m_ownerGlass = dynamic_cast<LiquidGlassWidget*>(parentWidget());
    if (QScreen* menuScreen = screen()) {
        delete m_scrim;
        m_scrim = new MenuScrim(menuScreen->geometry());
        m_scrim->setLevel(0.0);
        m_scrim->show();
        // 宿主卡片排在蒙版正上方；菜单随后再压在卡片上。
        if (m_ownerGlass)
            m_ownerElevation = m_ownerGlass->elevateForMenu(m_scrim->winId());
        else if (m_ownerWindow)
            NativeWindows::placeDragging(m_ownerWindow);
    }
    m_registeredWindow = winId();
    NativeWindows::registerGlassWindow(m_registeredWindow);
    m_surface->show();
    // 顺序固定为「压暗层 < 宿主卡片 < 菜单」。Qt 的置顶标志和弹出窗口的首次显示
    // 都要到自己那一轮事件循环走完才稳定，所以先按当前状态排一次，
    // 再在下一轮补一次最终层级，否则卡片和菜单会被压到蒙版下面。
    if (m_scrim) {
        applyMenuLayering();
        QTimer::singleShot(0, this, [this]() { applyMenuLayering(true); });
    } else {
        raise();
    }
    RenderDiagnostics::write(QStringLiteral("menu.show"),
        QStringLiteral("geometry=%1,%2 %3x%4 screenGeometry=%5,%6 %7x%8 dpr=%9 host=%10,%11 %12x%13")
            .arg(geometry().x()).arg(geometry().y()).arg(width()).arg(height())
            .arg(screen() ? screen()->geometry().x() : -1)
            .arg(screen() ? screen()->geometry().y() : -1)
            .arg(screen() ? screen()->geometry().width() : -1)
            .arg(screen() ? screen()->geometry().height() : -1)
            .arg(devicePixelRatioF())
            .arg(parentWidget() ? parentWidget()->frameGeometry().x() : -1)
            .arg(parentWidget() ? parentWidget()->frameGeometry().y() : -1)
            .arg(parentWidget() ? parentWidget()->frameGeometry().width() : -1)
            .arg(parentWidget() ? parentWidget()->frameGeometry().height() : -1));
    // 弹出后立刻补一次真实捕获：菜单这时还在淡入，替换背景不会被看见。
    // 有压暗层时跳过：否则会把压暗后的桌面采进菜单背景，菜单自己也会变暗。
    if (!m_scrim)
        QTimer::singleShot(0, this, &LiquidGlassMenu::refreshBackdrop);
    startOpenAnimation();
    if (m_liveBackdrop && !m_scrim)
        m_captureTimer.start();
}

void LiquidGlassMenu::applyMenuLayering(bool finalPass)
{
    if (!m_scrim)
        return;
    // 只用 HWND_TOPMOST 逐个抬：每次调用都把窗口放到置顶层最前面，
    // 于是调用顺序就是最终的上下顺序——压暗层 < 宿主卡片 < 菜单 < 玻璃表面。
    // 不要改成「插到某个窗口之后」：SetWindowPos 的锚点语义在置顶/普通两层交界处不可靠，
    // 之前那样写反而会把卡片和菜单排到蒙版下面。
    NativeWindows::placeDragging(m_scrim->winId());
    if (m_ownerWindow)
        NativeWindows::placeDragging(m_ownerWindow);
    NativeWindows::placeDragging(winId());
    // 玻璃表面一般是菜单窗口的子窗口，这一句只在它作为独立窗口时才真正生效。
    if (m_surface)
        NativeWindows::placeDragging(m_surface->winId());
    if (!finalPass)
        return;
    const NativeWindows::Stack stack = NativeWindows::snapshot();
    RenderDiagnostics::write(QStringLiteral("menu.layers"),
        QStringLiteral("scrim=%1 owner=%2 menu=%3 topmost=%4")
            .arg(NativeWindows::indexOf(stack, m_scrim->winId()))
            .arg(m_ownerWindow ? NativeWindows::indexOf(stack, m_ownerWindow) : -1)
            .arg(NativeWindows::indexOf(stack, winId()))
            .arg(NativeWindows::isTopmost(m_scrim->winId()) ? 1 : 0));
}

void LiquidGlassMenu::startOpenAnimation()
{
    if (!m_surface)
        return;
    if (!QApplication::isEffectEnabled(Qt::UI_AnimateMenu)) {
        applyOpenProgress(1.0);
        return;
    }
    if (!m_openTicker) {
        m_openTicker = new QTimer(this);
        m_openTicker->setTimerType(Qt::PreciseTimer);
        m_openTicker->setInterval(8);
        connect(m_openTicker, &QTimer::timeout, this, &LiquidGlassMenu::tickSpring);
    }
    // 与设置页下拉菜单同一套参数：带初速的弹簧，越过 1.0 后回弹一次再落定。
    m_progress = 0.0;
    m_velocity = SettingsAppearance::Motion::popupInitialVelocity;
    applyOpenProgress(0.0);
    m_openTicker->start();
}

void LiquidGlassMenu::tickSpring()
{
    constexpr qreal dt = 0.008;
    const qreal stiffness = SettingsAppearance::Motion::popupStiffnessIn;
    // 越过波峰后切到临界阻尼，只回弹一次，不会在最终尺寸下方再摆一次。
    const bool returning = m_progress > 1.0 && m_velocity <= 0;
    const qreal damping = returning ? 2.0 * std::sqrt(stiffness)
                                    : SettingsAppearance::Motion::popupDampingIn;
    const qreal before = m_progress;
    m_velocity += (stiffness * (1.0 - m_progress) - damping * m_velocity) * dt;
    m_progress += m_velocity * dt;
    applyOpenProgress(m_progress);
    if ((before > 1.0 && m_progress <= 1.0)
        || (qAbs(m_progress - 1.0) < 0.0015 && qAbs(m_velocity) < 0.04)) {
        m_openTicker->stop();
        applyOpenProgress(1.0);
    }
}

void LiquidGlassMenu::applyOpenProgress(qreal progress)
{
    m_progress = progress;
    // 透明度与设置页下拉菜单共用同一条映射：材质先于形状落定。
    const qreal fade = qBound<qreal>(0.0,
        SettingsAppearance::Motion::popupOpacityBias
            + progress * SettingsAppearance::Motion::popupOpacityGain, 1.0);
    if (m_surface) {
        m_surface->setSurfaceOpacity(fade);
        m_surface->setOpenProgress(progress);
    }
    // 压暗层与菜单同步解析：菜单淡入的同时桌面逐渐压暗。
    if (m_scrim)
        m_scrim->setLevel(fade);
}

QAction* LiquidGlassMenu::exec(const QPoint& position)
{
    return QMenu::exec(preparePopup(position));
}

void LiquidGlassMenu::popup(const QPoint& position)
{
    QMenu::popup(preparePopup(position));
}

QPoint LiquidGlassMenu::preparePopup(const QPoint& position)
{
    ++m_generation;
    m_cache.clear();
    m_capturePending = false;
    // 材质决定菜单外观，必须在算 sizeHint 之前应用。
    applyMenuStyle();
    QScreen* output = QGuiApplication::screenAt(position);
    if (!output)
        output = QGuiApplication::primaryScreen();
    // 窗口比面板每侧大 kPanelInset（留给回弹），原点反向补偿，面板仍对齐组件。
    QPoint origin = position - QPoint(kPanelInset, kPanelInset);
    if (output) {
        ensurePolished();
        const QSize extent = sizeHint();
        // 组件几何与 QScreen::geometry 在本应用里是同一套坐标（屏幕 2560x1440），
        // 不能再按 DPR 归一：那样会把屏幕高度缩成 960，把下方的可用空间算成负数，
        // 组件明明离屏幕底边还有几百像素也会被误判为「放不下」而翻到上方。
        // 弹窗可以盖住任务栏，所以用整块屏幕而不是工作区判断剩余空间。
        const QRect available = output->geometry();
        // 组件菜单只在组件的上方或下方悬停，不压在组件上，也不占用左右两侧。
        // 上下取剩得更多的一侧，左右优先与组件左缘对齐，右侧放不下时改为右对齐。
        // 组件可能有一截在屏幕外，按与可用区域相交的部分计算上下空间。
        const QRect host = parentWidget()
            ? parentWidget()->frameGeometry().intersected(available) : QRect();
        if (!host.isEmpty()) {
            constexpr int kGap = 8;
            // 上下空间按「面板」而不是「窗口」算：窗口四周多出的 kPanelInset 是留给回弹的。
            const int panelWidth = qMax(1, extent.width() - kPanelInset * 2);
            const int panelHeight = qMax(1, extent.height() - kPanelInset * 2);
            const int roomBelow = available.bottom() - (host.bottom() + kGap);
            const int roomAbove = (host.top() - kGap) - available.top();
            bool placeBelow = roomBelow >= panelHeight - 1;
            // 两边都放不下时选空间更大的一侧，随后仍会被夹到屏幕内。
            if (!placeBelow && roomAbove < panelHeight - 1)
                placeBelow = roomBelow >= roomAbove;
            // 默认与组件的右下角对齐：面板右缘贴住组件右缘，左侧放不下时才退回左对齐。
            const int rightAlignedX = host.right() + 1 - panelWidth;
            const bool rightAligned = rightAlignedX >= available.left();
            const int x = rightAligned ? rightAlignedX : host.left();
            const int y = placeBelow ? host.bottom() + 1 + kGap
                                     : host.top() - kGap - panelHeight;
            // 面板对齐组件，窗口再按留白反向偏移。
            origin = QPoint(x - kPanelInset, y - kPanelInset);
            m_openAnchor = QPointF(rightAligned ? 1.0 : 0.0, placeBelow ? 0.0 : 1.0);
        } else {
            // 没有宿主组件时退回指针锚点。
            m_openAnchor = QPointF(position.x() > available.left() + available.width() * .5 ? 1.0 : 0.0,
                                   position.y() > available.top() + available.height() * .5 ? 1.0 : 0.0);
        }
        origin.setX(qBound(available.left(), origin.x(), qMax(available.left(), available.right() - extent.width() + 1)));
        origin.setY(qBound(available.top(), origin.y(), qMax(available.top(), available.bottom() - extent.height() + 1)));
        const QRect captureArea(origin, extent);
        RenderDiagnostics::write(QStringLiteral("menu.popup"),
            QStringLiteral("position=%1,%2 host=%3,%4 %5x%6 extent=%7x%8 origin=%9,%10 anchor=%11,%12 available=%13,%14 %15x%16")
                .arg(position.x()).arg(position.y())
                .arg(host.x()).arg(host.y()).arg(host.width()).arg(host.height())
                .arg(extent.width()).arg(extent.height())
                .arg(origin.x()).arg(origin.y())
                .arg(m_openAnchor.x()).arg(m_openAnchor.y())
                .arg(available.x()).arg(available.y())
                .arg(available.width()).arg(available.height()));
        // 弹出路径上不做同步抓屏：它要抓取整个菜单区域并合成下层玻璃快照，
        // 会把菜单的出现推迟 100 ms 以上。先用壁纸切片立即出图，
        // 真实捕获在弹出后的第一帧由 refreshBackdrop 补上，期间正处在淡入过程中。
        const qreal scale = output->devicePixelRatio() * qMin(1.0f, m_renderScale);
        m_fallback = QImage(QSize(qMax(1, qRound(extent.width() * scale)),
                                 qMax(1, qRound(extent.height() * scale))), QImage::Format_RGB32);
        m_fallback.fill(AppSettings().value(QStringLiteral("appearance/darkMode"), false).toBool()
                            ? QColor(38, 47, 65) : QColor(242, 244, 248));
        QImage backdrop = LiquidGlassWidget::desktopWallpaper(captureArea, scale);
        if (backdrop.isNull())
            backdrop = m_fallback;
        // 回退背景不包含玻璃合成结果，防止反馈。
        m_fallback = backdrop;
        m_lastBackdrop = backdrop;
        createSurface();
        m_surface->setBackgroundImage(backdrop);
        m_captureTimer.setInterval(qBound(8, qRound(1000.0 / qMax(30.0, output->refreshRate())), 33));
    }
    return origin;
}

void LiquidGlassMenu::releaseBackdrop()
{
    m_captureTimer.stop();
    if (m_openTicker)
        m_openTicker->stop();
    ++m_generation;
    m_capturePending = false;
    NativeWindows::unregisterGlassWindow(m_registeredWindow);
    m_registeredWindow = 0;
    m_cache.clear();
    m_fallback = {};
    m_lastBackdrop = {};
}

void LiquidGlassMenu::spawnClosingGhost()
{
    if (m_ghostSpawned || !m_surface)
        return;
    m_ghostSpawned = true;
    const QImage frame = m_surface->surfaceSnapshot();
    // 子控件的 geometry() 是相对父窗口的（永远是 0,0），必须映射成全局坐标，
    // 否则回收动画会被放到屏幕左上角。
    const QRect globalRect(m_surface->mapToGlobal(QPoint(0, 0)), m_surface->size());
    RenderDiagnostics::write(QStringLiteral("menu.close"),
        QStringLiteral("frame=%1x%2 geometry=%3,%4 %5x%6")
            .arg(frame.width()).arg(frame.height())
            .arg(globalRect.x()).arg(globalRect.y())
            .arg(globalRect.width()).arg(globalRect.height()));
    if (frame.isNull()) {
        dropOwnerElevation();
        return;
    }
    auto* ghost = new MenuClosingGhost(frame, globalRect,
                                       m_openAnchor.x(), m_openAnchor.y(), m_scrim,
                                       m_ownerGlass, m_ownerElevation, m_ownerWindow);
    m_scrim = nullptr;   // 所有权转交
    m_ownerGlass = nullptr;
    m_ownerElevation = 0;
    m_ownerWindow = 0;
    RenderDiagnostics::write(QStringLiteral("menu.ghost"),
        QStringLiteral("start geometry=%1,%2 %3x%4 dpr=%5 scrim=%6 anchor=%7,%8")
            .arg(globalRect.x()).arg(globalRect.y())
            .arg(globalRect.width()).arg(globalRect.height())
            .arg(m_surface->devicePixelRatioF())
            .arg(ghost->scrimAttached() ? 1 : 0)
            .arg(m_openAnchor.x(), 0, 'f', 2).arg(m_openAnchor.y(), 0, 'f', 2));
    ghost->startFade();
}

void LiquidGlassMenu::dropOwnerElevation()
{
    if (m_scrim) {
        delete m_scrim;
        m_scrim = nullptr;
    }
    if (m_ownerGlass) {
        m_ownerGlass->dropMenuElevation(m_ownerElevation);
        m_ownerGlass = nullptr;
    } else if (m_ownerWindow) {
        NativeWindows::placeDesktop(m_ownerWindow);
    }
    m_ownerWindow = 0;
    m_ownerElevation = 0;
}

void LiquidGlassMenu::hideEvent(QHideEvent* event)
{
    // 正常关闭路径。
    spawnClosingGhost();
    releaseBackdrop();
    QMenu::hideEvent(event);
}

void LiquidGlassMenu::resizeEvent(QResizeEvent* event)
{
    QMenu::resizeEvent(event);
    if (m_surface)
        m_surface->fit(rect());
}

void LiquidGlassMenu::moveEvent(QMoveEvent* event)
{
    QMenu::moveEvent(event);
    if (isVisible() && m_registeredWindow)
        refreshBackdrop();
}

void LiquidGlassMenu::refreshBackdrop()
{
    if (!isVisible() || !m_registeredWindow || m_capturePending)
        return;
    m_capturePending = true;
    const quint64 generation = m_generation;
    const QRect area = geometry();
    const qreal scale = devicePixelRatioF() * m_surface->renderScale();
    DesktopCapture::request(area, scale, m_registeredWindow, this,
        [this, generation, area](DesktopCapture::Frame frame) {
            if (generation != m_generation)
                return;
            m_capturePending = false;
            if (!isVisible() || area != geometry())
                return;
            QImage backdrop = m_cache.merge(std::move(frame), [this]() { return m_fallback; });
            LiquidGlassWidget::compositeDesktopGlass(backdrop, area, m_registeredWindow);
            if (!backdrop.isNull() && backdrop != m_lastBackdrop) {
                m_lastBackdrop = backdrop;
                m_surface->setBackgroundImage(backdrop);
            }
        });
}

void LiquidGlassMenu::paintEvent(QPaintEvent*)
{
    // 子窗口独立绘制，父窗口不能再次调度子窗口更新，以免形成重绘循环。
}

bool LiquidGlassMenu::event(QEvent* event)
{
    const bool handled = QMenu::event(event);
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseMove:
    case QEvent::Leave:
    case QEvent::ActionChanged:
    case QEvent::ActionAdded:
    case QEvent::ActionRemoved:
        if (m_surface)
            m_surface->update();
        break;
    default:
        break;
    }
    return handled;
}

void LiquidGlassMenu::paintContents(QPainter& painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing,
        !(font().styleStrategy() & QFont::NoAntialias));
    painter.setFont(font());
    // QMenu 的行矩形已经含了面板外圈的内边距（kPanelInset），画笔也已按面板原点平移，
    // 这里必须把行坐标退回面板坐标系，否则内容会整体多偏一个内边距：
    // 表现为首行被撑高、分割线偏下、第二行被面板底边裁掉。
    painter.translate(-kPanelInset, -kPanelInset);

    QVector<QRectF> itemRows;
    QVector<QRectF> separatorRows;
    for (const QAction* action : actions()) {
        if (!action->isVisible())
            continue;
        const QRectF geometry = actionGeometry(const_cast<QAction*>(action));
        if (geometry.isEmpty())
            continue;
        if (action->isSeparator())
            separatorRows.append(geometry);
        else
            itemRows.append(geometry);
    }

    if (m_style.rowDividers) {
        // 磨砂玻璃按 iPadOS：整个菜单是同一种底色，行与行之间只有一条 1px 发丝线，
        // 不再给「分隔带」铺底板、也不再给行铺亮底，否则线间会多出一条灰带。
        painter.setPen(QPen(CardInk::chrome(QColor(255, 255, 255, 34)), 1));
        for (int i = 0; i + 1 < itemRows.size(); ++i) {
            const QRectF& geometry = itemRows.at(i);
            painter.drawLine(QPointF(geometry.left(), geometry.bottom()),
                             QPointF(geometry.right(), geometry.bottom()));
        }
    } else {
        // 液态玻璃只在分组之间画一条内缩短线。
        painter.setPen(QPen(CardInk::chrome(QColor(255, 255, 255, 30)), 1));
        for (const QRectF& separator : separatorRows)
            painter.drawLine(QPointF(20, separator.center().y()),
                             QPointF(width() - 20, separator.center().y()));
    }

    for (const QAction* action : actions()) {
        const QRectF item = actionGeometry(const_cast<QAction*>(action));
        if (!action->isVisible() || item.isEmpty() || action->isSeparator())
            continue;
        const bool selected = action == activeAction() && action->isEnabled();
        if (selected) {
            if (m_style.pillHighlight) {
                // 液态玻璃：内缩的圆角高亮块，带一条上沿细线。
                QLinearGradient highlight(item.topLeft(), item.bottomLeft());
                highlight.setColorAt(0, CardInk::chrome(QColor(255, 255, 255, 43)));
                highlight.setColorAt(1, CardInk::chrome(QColor(220, 237, 255, 22)));
                painter.setBrush(highlight);
                painter.setPen(QPen(CardInk::chrome(QColor(255, 255, 255, 37)), .8));
                painter.drawRoundedRect(item.adjusted(.5, .5, -.5, -.5), 9, 9);
            } else {
                // 磨砂玻璃：行底顶到菜单边缘，形状由菜单圆角裁出（没有内缩、没有每行圆角）。
                painter.setPen(Qt::NoPen);
                painter.setBrush(CardInk::chrome(QColor(255, 255, 255, 30)));
                painter.drawRect(item);
            }
        }
        const auto icon = static_cast<Icon>(action->property(kIconProperty).toInt());
        // 删除项用系统红（iPadOS 的移除行）：亮色 (255,59,48)、暗色 (255,69,58)，
        // 不再用 accent() 把红色再压暗成淡粉。
        const QColor color = !action->isEnabled() ? CardInk::chrome(QColor(235, 240, 250, 88))
            : icon == Icon::Remove
                ? (CardInk::dark() ? QColor(255, 69, 58) : QColor(255, 59, 48))
                : CardInk::chrome(QColor(246, 249, 255));
        painter.setPen(color);
        drawIcon(painter, QRectF(item.left() + 11, item.center().y() - 8, 16, 16), icon);
        ResponsiveLayout::drawSingleLine(painter, item.adjusted(36, 0, -14, 0),
                                        Qt::AlignLeft | Qt::AlignVCenter, action->text());
    }
    painter.restore();
}
