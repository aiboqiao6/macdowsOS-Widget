#include "ui/widgetlibrarydialog.h"
#include "widgets/desktopwidget.h"
#include "media/systemmediasession.h"
#include "rendering/responsivelayout.h"
#include "app/appsettings.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QCache>
#include <QIconEngine>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QOperatingSystemVersion>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

// 图库表面与内容分别缩放，最终窗口尺寸受桌面工作区限制。
constexpr int kGalleryWidth = 1320 * 3 / 2;
constexpr int kGalleryHeight = 800 * 3 / 2;
constexpr qreal kGalleryContentZoom = 1.5;

void paintNavIcon(QPainter& painter, const QRectF& bounds, int category)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = qMin(bounds.width(), bounds.height()) * .1;
    const QRectF canvas = bounds.adjusted(inset, inset, -inset, -inset);
    const ResponsiveLayout::Metrics metrics(canvas);
    const QColor ink = category == 1 ? QColor(111, 232, 157, 245)
                      : category == 2 ? QColor(122, 194, 255, 245)
                      : category == 3 ? QColor(220, 229, 244, 245)
                      : category == 4 ? QColor(229, 157, 245, 245)
                                      : QColor(198, 185, 255, 245);
    painter.setPen(QPen(ink, metrics.stroke(painter, .075), Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (category == 1) {
        painter.drawRoundedRect(metrics.rect(.16, .30, .62, .40), metrics.size(.08), metrics.size(.08));
        painter.drawLine(metrics.point(.78, .42), metrics.point(.88, .42));
        painter.drawLine(metrics.point(.78, .58), metrics.point(.88, .58));
    } else if (category == 2) {
        painter.drawArc(metrics.rect(.20, .38, .40, .34), 35 * 16, 250 * 16);
        painter.drawArc(metrics.rect(.42, .27, .38, .45), 120 * 16, 220 * 16);
        painter.drawLine(metrics.point(.18, .72), metrics.point(.84, .72));
        painter.drawLine(metrics.point(.28, .82), metrics.point(.72, .82));
    } else if (category == 3) {
        painter.drawEllipse(metrics.rect(.15, .15, .70, .70));
        painter.drawLine(metrics.point(.50, .50), metrics.point(.50, .29));
        painter.drawLine(metrics.point(.50, .50), metrics.point(.67, .61));
    } else if (category == 4) {
        painter.drawLine(metrics.point(.17, .27), metrics.point(.50, .43));
        painter.drawLine(metrics.point(.83, .27), metrics.point(.50, .43));
        painter.drawLine(metrics.point(.17, .27), metrics.point(.17, .73));
        painter.drawLine(metrics.point(.83, .27), metrics.point(.83, .73));
        painter.drawLine(metrics.point(.17, .73), metrics.point(.50, .57));
        painter.drawLine(metrics.point(.83, .73), metrics.point(.50, .57));
    } else if (category == 5) {
        painter.drawLine(metrics.point(.43, .23), metrics.point(.43, .70));
        painter.drawLine(metrics.point(.43, .23), metrics.point(.73, .15));
        painter.drawLine(metrics.point(.73, .15), metrics.point(.73, .61));
        painter.drawEllipse(metrics.rect(.23, .62, .20, .15));
        painter.drawEllipse(metrics.rect(.53, .53, .20, .15));
    } else {
        painter.drawRoundedRect(metrics.rect(.16, .16, .28, .28), 2, 2);
        painter.drawRoundedRect(metrics.rect(.56, .16, .28, .28), 2, 2);
        painter.drawRoundedRect(metrics.rect(.16, .56, .28, .28), 2, 2);
        painter.drawRoundedRect(metrics.rect(.56, .56, .28, .28), 2, 2);
    }
    painter.restore();
}

class NavIconEngine final : public QIconEngine
{
public:
    explicit NavIconEngine(int category) : m_category(category) {}
    QIconEngine* clone() const override { return new NavIconEngine(m_category); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        paintNavIcon(*painter, rect, m_category);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode, QIcon::State, qreal scale) override
    {
        QPixmap result(size * scale);
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paintNavIcon(painter, QRectF(QPointF(), QSizeF(size)), m_category);
        return result;
    }
private:
    int m_category;
};

QIcon makeNavIcon(int category)
{
    return QIcon(new NavIconEngine(category));
}

QCache<QString, QImage>& previewCache()
{
    static QCache<QString, QImage> cache(32 * 1024);
    return cache;
}

class DragTile final : public QFrame
{
public:
    DragTile(const QString& title, const QString& subtitle, int kind,
             int variant, int category, WidgetLibraryDialog* owner, qreal scale)
        : QFrame(owner), m_title(title), m_subtitle(subtitle), m_cardKind(kind),
          m_cardVariant(variant), m_category(category), m_owner(owner), m_scale(scale)
    {
        setObjectName(QStringLiteral("widgetTile"));
        setProperty("tileTitle", title);
        setFixedSize(qRound(220 * m_scale), qRound(176 * m_scale));
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::OpenHandCursor);
        m_press = QPoint(-1, -1);
        // 复用真实组件内容绘制预览，按当前屏幕像素密度缓存，不显示 GL 窗口。
        refreshPreview();
        if (m_cardKind == int(DesktopWidget::CardKind::Music)) {
            connect(SystemMediaSession::instance(), &SystemMediaSession::changed, this, [this] {
                if (isVisible()) refreshPreview();
            });
        }
    }

    void setScale(qreal scale)
    {
        if (qFuzzyCompare(m_scale, scale))
            return;
        m_scale = scale;
        setFixedSize(qRound(220 * m_scale), qRound(176 * m_scale));
        refreshPreview();
    }

    void refreshPreview()
    {
        const QSize previewSize(204, 128);
        const qreal dpr = devicePixelRatioF() * m_scale;
        const QString key = QStringLiteral("%1:%2:%3x%4:%5:%6")
            .arg(m_cardKind).arg(m_cardVariant).arg(previewSize.width()).arg(previewSize.height())
            .arg(dpr, 0, 'g', 12).arg(DesktopWidget::globalFontSmoothing());
        if (m_cardKind == int(DesktopWidget::CardKind::Music))
            previewCache().remove(key);
        if (const auto* cached = previewCache().object(key)) {
            m_preview = *cached;
        } else {
            m_preview = DesktopWidget::renderPreview(
                static_cast<DesktopWidget::CardKind>(m_cardKind), m_cardVariant, previewSize, dpr);
            const int cost = qMax(1, int((m_preview.sizeInBytes() + 1023) / 1024));
            previewCache().insert(key, new QImage(m_preview), cost);
        }
        update();
    }

protected:
    bool event(QEvent* event) override
    {
        const bool handled = QFrame::event(event);
        if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show)
            refreshPreview();
        return handled;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.scale(m_scale, m_scale);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing,
                        DesktopWidget::globalFontSmoothing() > 0);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // 图库共用一个玻璃表面，预览项不叠加额外外框。
        const QRectF preview(8, 2, 204, 128);
        if (!m_preview.isNull()) {
            const QTransform device = painter.deviceTransform();
            const QPointF origin = preview.center()
                - QPointF(m_preview.deviceIndependentSize().width() * .5,
                          m_preview.deviceIndependentSize().height() * .5);
            const QPoint pixelOrigin = device.map(origin).toPoint();
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(device.inverted().map(QPointF(pixelOrigin)), m_preview);
            painter.restore();
        } else {
            painter.setPen(QColor(220, 225, 235));
            painter.drawText(preview, Qt::AlignCenter, QStringLiteral("预览暂不可用"));
        }

        painter.setPen(QColor(247, 249, 253));
        QFont footerFont = qApp->font();
        footerFont.setPixelSize(14);
        footerFont.setWeight(QFont::DemiBold);
        DesktopWidget::applyFontSmoothing(footerFont);
        painter.setFont(footerFont);
        ResponsiveLayout::drawSingleLine(painter, QRectF(10, 134, 200, 20),
                                   Qt::AlignCenter | Qt::AlignVCenter, m_title);
        painter.setPen(QColor(173, 186, 207));
        QFont subtitleFont = qApp->font();
        subtitleFont.setPixelSize(12);
        DesktopWidget::applyFontSmoothing(subtitleFont);
        painter.setFont(subtitleFont);
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(10, 154, 200, 17),
            Qt::AlignCenter | Qt::AlignVCenter,
            QFontMetrics(painter.font()).elidedText(m_subtitle, Qt::ElideRight, 192));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_press = event->pos();
            m_dragging = false;
            qApp->installEventFilter(this);
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

    void cancelDrag()
    {
        const bool wasDragging = m_dragging;
        m_press = QPoint(-1, -1);
        m_dragging = false;
        qApp->removeEventFilter(this);
        setCursor(Qt::OpenHandCursor);
        if (wasDragging)
            m_owner->notifyWidgetDragFinished(m_cardKind * 100 + m_cardVariant);
        m_owner->notifyWidgetDragPreview(m_cardKind * 100 + m_cardVariant, QCursor::pos(), false);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancelDrag();
            return true;
        }
        if (event->type() == QEvent::ApplicationDeactivate
            || (watched == this && event->type() == QEvent::UngrabMouse))
            cancelDrag();
        return QFrame::eventFilter(watched, event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            const bool dropped = m_dragging && m_press.x() >= 0;
            const QPoint position = event->globalPosition().toPoint();
            cancelDrag();
            if (dropped && !m_owner->geometry().contains(position))
                m_owner->notifyWidgetDropped(m_cardKind * 100 + m_cardVariant, position);
            event->accept();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_press.x() < 0 || !(event->buttons() & Qt::LeftButton)) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        if (!m_dragging && (event->pos() - m_press).manhattanLength() < QApplication::startDragDistance())
            return;
        if (!m_dragging) {
            m_dragging = true;
            m_owner->notifyWidgetDragStarted(m_cardKind * 100 + m_cardVariant);
        }
        const QPoint position = event->globalPosition().toPoint();
        m_owner->notifyWidgetDragPreview(m_cardKind * 100 + m_cardVariant, position, !m_owner->geometry().contains(position));
        event->accept();
    }

private:
    QString m_title;
    QString m_subtitle;
    int m_cardKind = 0;
    int m_cardVariant = 0;
    int m_category = -1;
    QImage m_preview;
    QPoint m_press;
    bool m_dragging = false;
    WidgetLibraryDialog* m_owner = nullptr;
    qreal m_scale = 1.0;
};

}

void WidgetLibraryDialog::refreshFontRendering()
{
    previewCache().clear();
    if (!qApp)
        return;
    for (QWidget* widget : qApp->allWidgets()) {
        auto* library = qobject_cast<WidgetLibraryDialog*>(widget);
        if (!library)
            continue;
        for (QFrame* tile : std::as_const(library->m_tiles))
            static_cast<DragTile*>(tile)->refreshPreview();
        library->update();
    }
}

WidgetLibraryDialog::WidgetLibraryDialog(QWidget* parent)
    : LiquidGlassWidget(parent)
{
    setWindowTitle(QStringLiteral("小组件"));
    // 图库复用玻璃表面但作为交互面板置于应用上方，背景由 prepareBackdrop 提供。
    setPanelWindow();
    setDesktopLayerEnabled(false);
    setDesktopCaptureEnabled(false);
    const AppSettings settings;
    m_interfaceScale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    setLiveBackdropEnabled(
        settings.value(QStringLiteral("appearance/liveBackdrop"), true).toBool());
    setGlassMargins(0);
    setGlassRadius(26.0 * m_interfaceScale);
    setRenderScale(float(qBound<qreal>(0.5,
        settings.value(QStringLiteral("performance/renderScale"), 0.60).toDouble(), 1.5)));
    Qt::WindowFlags flags = windowFlags();
    flags.setFlag(Qt::WindowStaysOnBottomHint, false);
    flags.setFlag(Qt::WindowStaysOnTopHint, true);
    flags.setFlag(Qt::WindowDoesNotAcceptFocus, false);
    flags.setFlag(Qt::Tool, true);
    flags.setFlag(Qt::FramelessWindowHint, true);
    setWindowFlags(flags);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    m_renderTimer.setInterval(16);
    connect(&m_renderTimer, &QTimer::timeout, this, [this]() {
        // 动画与内存裁剪在界面线程更新，桌面捕获由独立定时器调度。
        if (isVisible()) {
            updateBackdropFrame();
            update();
        }
    });
    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
    m_liveBackdropTimer.setSingleShot(false);
    // 实时捕获独立于短暂的面板动画，显示时按实际显示器更新间隔。
    m_liveBackdropTimer.setInterval(qMax(1, activeDisplayInterval() / 2));
    connect(&m_liveBackdropTimer, &QTimer::timeout,
            this, &WidgetLibraryDialog::refreshBackdrop);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // 图库比桌面卡片大，清除继承的最大尺寸约束。
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setMinimumSize(windowScaled(320), windowScaled(240));
    resize(windowScaled(kGalleryWidth), windowScaled(kGalleryHeight));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* body = new QWidget(this);
    body->setAttribute(Qt::WA_TranslucentBackground);
    body->setAttribute(Qt::WA_NoSystemBackground);
    body->setAutoFillBackground(false);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto* sidebar = new QFrame(body);
    m_sidebar = sidebar;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(270);
    auto* sideLayout = new QVBoxLayout(sidebar);
    m_sideLayout = sideLayout;
    sideLayout->setContentsMargins(18, 22, 16, 18);
    sideLayout->setSpacing(5);

    auto* search = new QLineEdit(sidebar);
    m_search = search;
    search->setPlaceholderText(QStringLiteral("搜索小组件"));
    search->setClearButtonEnabled(true);
    search->setFixedHeight(36);
    search->setStyleSheet(QStringLiteral(
        "QLineEdit { border:1px solid rgba(255,255,255,75); border-radius:18px; "
        "padding:0 12px; color:#f7fbff; background:rgba(238,246,253,54); font-size:15px; }"
        "QLineEdit:focus { border:1px solid rgba(255,255,255,145); background:rgba(238,246,253,74); }"));
    sideLayout->addWidget(search);

    const QStringList navLabels = { QStringLiteral("所有小组件"), QStringLiteral("电池"),
                                     QStringLiteral("天气"), QStringLiteral("时钟"),
                                     QStringLiteral("词典"), QStringLiteral("音乐") };
    for (int i = 0; i < navLabels.size(); ++i) {
        auto* button = new QPushButton(navLabels.at(i), sidebar);
        button->setObjectName(i == 0 ? QStringLiteral("navSelected") : QStringLiteral("navButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFlat(true);
        button->setFixedHeight(39);
        button->setIcon(makeNavIcon(i));
        button->setIconSize(QSize(27, 27));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("navCategory", i);
        m_navButtons.append(button);
        sideLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i]() { selectCategory(i); });
    }
    sideLayout->addStretch(1);
    connect(search, &QLineEdit::textChanged, this, &WidgetLibraryDialog::applyTileFilter);

    auto* content = new QWidget(body);
    content->setAttribute(Qt::WA_TranslucentBackground);
    content->setAttribute(Qt::WA_NoSystemBackground);
    content->setAutoFillBackground(false);
    auto* contentLayout = new QVBoxLayout(content);
    m_contentLayout = contentLayout;
    contentLayout->setContentsMargins(30, 23, 25, 12);
    contentLayout->setSpacing(12);
    auto* sectionTitle = new QLabel(QStringLiteral("推荐"), content);
    m_sectionTitle = sectionTitle;
    sectionTitle->setObjectName(QStringLiteral("sectionTitle"));
    contentLayout->addWidget(sectionTitle);

    auto* scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAttribute(Qt::WA_TranslucentBackground);
    auto* scrollHost = new QWidget(scroll);
    scrollHost->setAttribute(Qt::WA_TranslucentBackground);
    auto* tileLayout = new QGridLayout(scrollHost);
    m_tileLayout = tileLayout;
    tileLayout->setContentsMargins(0, 0, 0, 12);
    tileLayout->setHorizontalSpacing(14);
    tileLayout->setVerticalSpacing(14);
    const auto addTile = [this, tileLayout](const QString& title, const QString& subtitle,
                                             int kind, int variant, int category) {
        auto* tile = new DragTile(title, subtitle, kind, variant, category,
                                  this, m_interfaceScale * kGalleryContentZoom);
        tile->setProperty("tileKind", kind);
        tile->setProperty("tileVariant", variant);
        tile->setProperty("tileCategory", category);
        tile->setProperty("tileSearch", (title + QStringLiteral(" ") + subtitle).toLower());
        m_tiles.append(tile);
    };
    addTile(QStringLiteral("状态"), QStringLiteral("查看本机和已连接蓝牙配件的电量。"), 0, 0, 1);
    addTile(QStringLiteral("状态"), QStringLiteral("查看本机和已连接蓝牙配件的电量。"), 0, 1, 1);
    addTile(QStringLiteral("状态"), QStringLiteral("查看本机和已连接蓝牙配件的电量。"), 0, 2, 1);
    addTile(QStringLiteral("天气 · 紧凑"), QStringLiteral("温度与天气"), 1, 0, 2);
    addTile(QStringLiteral("天气 · 详细"), QStringLiteral("地点与状态"), 1, 1, 2);
    addTile(QStringLiteral("天气 · 预报"), QStringLiteral("最高与最低温度"), 1, 2, 2);
    addTile(QStringLiteral("数字时钟"), QStringLiteral("显示当前时间"), 2, 2, 3);
    addTile(QStringLiteral("时钟 I"), QStringLiteral("显示当前时间"), 2, 0, 3);
    addTile(QStringLiteral("数字时钟 · 横向"), QStringLiteral("2×1 显示当前时间"), 2, 8, 3);
    addTile(QStringLiteral("每日单词"), QStringLiteral("随机单词与释义"), 3, 0, 4);
    addTile(QStringLiteral("词典详情"), QStringLiteral("单词、音标与释义"), 3, 1, 4);
    addTile(QStringLiteral("词典卡片"), QStringLiteral("轻量词汇提醒"), 3, 2, 4);
    addTile(QStringLiteral("音乐 · 紧凑"), QStringLiteral("系统播放状态与封面"), 4, 0, 5);
    addTile(QStringLiteral("音乐 · 横向"), QStringLiteral("曲目信息、进度与播放控制"), 4, 1, 5);
    tileLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setWidget(scrollHost);
    contentLayout->addWidget(scroll, 1);
    bodyLayout->addWidget(sidebar);
    bodyLayout->addWidget(content, 1);
    root->addWidget(body, 1);

    auto* footer = new QFrame(this);
    m_footer = footer;
    footer->setObjectName(QStringLiteral("footer"));
    footer->setFixedHeight(70);
    auto* footerLayout = new QHBoxLayout(footer);
    m_footerLayout = footerLayout;
    footerLayout->setContentsMargins(24, 0, 23, 0);
    auto* footerHint = new QLabel(QStringLiteral("将小组件拖放到桌面…"), footer);
    footerHint->setObjectName(QStringLiteral("footerHint"));
    footerLayout->addWidget(footerHint);
    footerLayout->addStretch(1);
    auto* done = new QPushButton(QStringLiteral("完成"), footer);
    m_doneButton = done;
    done->setObjectName(QStringLiteral("doneButton"));
    done->setFixedHeight(36);
    footerLayout->addWidget(done);
    root->addWidget(footer);
    connect(done, &QPushButton::clicked, this, &WidgetLibraryDialog::closeGallery);

    setInterfaceScale(m_interfaceScale);
    randomizeRecommendations();
    applyTileFilter();

}

int WidgetLibraryDialog::scaled(int value) const
{
    return qMax(1, qRound(value * m_interfaceScale * kGalleryContentZoom));
}

int WidgetLibraryDialog::windowScaled(int value) const
{
    return qMax(1, qRound(value * m_interfaceScale));
}

int WidgetLibraryDialog::tileColumnCount() const
{
    const int sidebarWidth = width() < scaled(650) ? 0 : qMin(scaled(270), width() / 4);
    return qBound(1, (width() - sidebarWidth - scaled(55) + scaled(14))
                     / scaled(234), 4);
}

void WidgetLibraryDialog::setInterfaceScale(qreal scale)
{
    const qreal newScale = qBound<qreal>(0.40, scale, 2.0);
    if (m_scaleApplied && qFuzzyCompare(m_interfaceScale, newScale))
        return;
    if (m_scaleApplied)
        previewCache().clear();
    m_scaleApplied = true;
    if (m_slideAnimation && m_slideAnimation->state() == QAbstractAnimation::Running)
        m_slideAnimation->stop();
    m_interfaceScale = newScale;
    setGlassRadius(26.0 * m_interfaceScale);
    // 圆角半径不能大于控件高度的一半，避免小数缩放时 QSS 回退为方角。
    const int controlRadius = qMin(scaled(18), scaled(36) / 2);

    const QString style = QStringLiteral(
        "WidgetLibraryDialog { background:transparent; color:#f7f9fd; border:0; }"
        "QFrame#sidebar, QFrame#footer { background:transparent; border:0; }"
        "QLabel#sectionTitle { color:#f8fbff; font-size:%1px; font-weight:600; }"
        "QLabel#footerHint { color:rgba(246,250,255,225); font-size:%2px; }"
        "QPushButton#navButton, QPushButton#navSelected { text-align:left; border:0; border-radius:%3px; padding-left:%4px; color:rgba(246,249,253,230); font-size:%2px; }"
        "QPushButton#navButton:hover { background:rgba(255,255,255,48); }"
        "QPushButton#navSelected { background:rgba(255,255,255,104); color:#ffffff; font-weight:600; }"
        "QPushButton#doneButton { background:#1684f7; border:1px solid rgba(255,255,255,90); border-radius:%5px; color:white; font-size:%2px; font-weight:600; padding:0 %6px; }"
        "QPushButton#doneButton:hover { background:#3195fb; }"
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget { background:transparent; border:0; }"
        "QScrollBar:vertical { width:%7px; background:transparent; }"
        "QScrollBar::handle:vertical { background:rgba(255,255,255,96); border-radius:%8px; min-height:%9px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
        .arg(scaled(19)).arg(scaled(15)).arg(scaled(12)).arg(scaled(10))
        .arg(controlRadius).arg(scaled(23)).arg(scaled(7)).arg(scaled(3))
        .arg(scaled(35));
    setStyleSheet(style);
    if (m_sidebar)
        m_sidebar->setFixedWidth(scaled(270));
    if (m_sideLayout) {
        m_sideLayout->setContentsMargins(scaled(18), scaled(22), scaled(16), scaled(18));
        m_sideLayout->setSpacing(scaled(5));
    }
    if (m_search) {
        m_search->setFixedHeight(scaled(36));
        m_search->setStyleSheet(QStringLiteral(
            "QLineEdit { border:1px solid rgba(255,255,255,75); border-radius:%1px; "
            "padding:0 %2px; color:#f7fbff; background:rgba(238,246,253,54); font-size:%3px; }"
            "QLineEdit:focus { border:1px solid rgba(255,255,255,145); background:rgba(238,246,253,74); }")
            .arg(controlRadius).arg(scaled(12)).arg(scaled(15)));
    }
    for (QPushButton* button : std::as_const(m_navButtons)) {
        button->setFixedHeight(scaled(39));
        button->setIconSize(QSize(scaled(27), scaled(27)));
    }
    if (m_contentLayout) {
        m_contentLayout->setContentsMargins(scaled(30), scaled(23), scaled(25), scaled(12));
        m_contentLayout->setSpacing(scaled(12));
    }
    if (m_tileLayout) {
        m_tileLayout->setContentsMargins(0, 0, 0, scaled(12));
        m_tileLayout->setHorizontalSpacing(scaled(14));
        m_tileLayout->setVerticalSpacing(scaled(14));
    }
    for (QFrame* tile : std::as_const(m_tiles))
        static_cast<DragTile*>(tile)->setScale(m_interfaceScale * kGalleryContentZoom);
    if (m_footer)
        m_footer->setFixedHeight(scaled(70));
    if (m_footerLayout)
        m_footerLayout->setContentsMargins(scaled(24), 0, scaled(23), 0);
    if (m_doneButton)
        m_doneButton->setFixedHeight(scaled(36));

    QScreen* screen = isVisible() ? QGuiApplication::screenAt(geometry().center())
                                  : QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    const int maxWidth = screen ? qMax(1, area.width() - 32) : QWIDGETSIZE_MAX;
    const int maxHeight = screen ? qMax(1, area.height() - 32) : QWIDGETSIZE_MAX;
    setMinimumSize(qMin(windowScaled(320), maxWidth),
                   qMin(windowScaled(240), maxHeight));
    resize(qMin(windowScaled(kGalleryWidth), maxWidth),
           qMin(windowScaled(kGalleryHeight), maxHeight));
    if (isVisible() && screen) {
        const int targetX = area.left() + (area.width() - width()) / 2;
        const int targetY = area.bottom() - height() - 8;
        move(targetX, targetY);
        m_backdropRect = geometry();
        m_backdropCaptureRect = QRect(targetX, targetY, width(),
                                      qMax(1, area.bottom() - targetY + 1));
        QImage backdrop = captureDesktopComposite(screen, m_backdropCaptureRect,
                                                   this, renderScale());
        if (!backdrop.isNull()) {
            m_backdropCanvas = std::move(backdrop);
            m_lastBackdropGeometry = QRect();
            updateBackdropFrame();
        }
        if (liveBackdropEnabled()) {
            m_liveBackdropTimer.setInterval(qMax(1, activeDisplayInterval() / 2));
            m_liveBackdropTimer.start();
        }
    }
    applyTileFilter();
}

void WidgetLibraryDialog::randomizeRecommendations()
{
    m_recommendedTiles.clear();
    if (m_tiles.isEmpty())
        return;
    // 随机推荐时保持类型多样性。
    QList<int> candidates;
    for (int i = 0; i < m_tiles.size(); ++i)
        candidates.append(i);
    for (int i = candidates.size() - 1; i > 0; --i) {
        const int j = int(QRandomGenerator::global()->bounded(i + 1));
        candidates.swapItemsAt(i, j);
    }
    QSet<int> categories;
    for (int index : candidates) {
        const int category = m_tiles.at(index)->property("tileCategory").toInt();
        if (categories.size() < 4 && categories.contains(category))
            continue;
        m_recommendedTiles.append(index);
        categories.insert(category);
        if (m_recommendedTiles.size() == 6)
            break;
    }
    for (int index : candidates) {
        if (m_recommendedTiles.size() == 6)
            break;
        if (!m_recommendedTiles.contains(index))
            m_recommendedTiles.append(index);
    }
}

void WidgetLibraryDialog::selectCategory(int category)
{
    m_selectedCategory = category == 0 ? -1 : category;
    for (int i = 0; i < m_navButtons.size(); ++i) {
        auto* button = m_navButtons.at(i);
        button->setObjectName(i == category ? QStringLiteral("navSelected")
                                            : QStringLiteral("navButton"));
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    if (m_sectionTitle)
        m_sectionTitle->setText(category == 0 ? QStringLiteral("推荐")
                                              : (category == 1 ? QStringLiteral("电池")
                                                 : category == 2 ? QStringLiteral("天气")
                                                    : category == 3 ? QStringLiteral("时钟")
                                                    : category == 4 ? QStringLiteral("词典")
                                                                    : QStringLiteral("音乐")));
    if (category == 0)
        randomizeRecommendations();
    applyTileFilter();
}

void WidgetLibraryDialog::applyTileFilter()
{
    if (!m_tileLayout)
        return;
    const QString query = m_search ? m_search->text().trimmed().toLower() : QString();
    QList<QFrame*> visible;
    for (int i = 0; i < m_tiles.size(); ++i) {
        QFrame* tile = m_tiles.at(i);
        const int category = tile->property("tileCategory").toInt();
        const bool categoryMatch = m_selectedCategory < 0
            ? m_recommendedTiles.contains(i)
            : category == m_selectedCategory;
        const bool queryMatch = query.isEmpty()
            || tile->property("tileSearch").toString().contains(query);
        tile->setVisible(categoryMatch && queryMatch);
        if (categoryMatch && queryMatch)
            visible.append(tile);
        m_tileLayout->removeWidget(tile);
    }
    const int columns = tileColumnCount();
    m_tileColumns = columns;
    for (int i = 0; i < visible.size(); ++i)
        m_tileLayout->addWidget(visible.at(i), i / columns, i % columns);
    m_tileLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

void WidgetLibraryDialog::mousePressEvent(QMouseEvent* event)
{
    // 预览项处理鼠标捕获，图库背景保持固定。
    event->accept();
}

void WidgetLibraryDialog::mouseMoveEvent(QMouseEvent* event)
{
    event->accept();
}

void WidgetLibraryDialog::mouseReleaseEvent(QMouseEvent* event)
{
    event->accept();
}

bool WidgetLibraryDialog::event(QEvent* event)
{
    // 图库失去激活时立即关闭，避免临时置顶面板滞留在屏幕上。
    if (event->type() == QEvent::WindowDeactivate && !m_closing) {
        closeGallery();
        return true;
    }
    return LiquidGlassWidget::event(event);
}

void WidgetLibraryDialog::prepareBackdrop()
{
    selectCategory(0);
    preparePanelOpening();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    const int targetWidth = qMin(windowScaled(kGalleryWidth), qMax(1, area.width() - 32));
    const int targetHeight = qMin(windowScaled(kGalleryHeight), qMax(1, area.height() - 32));
    setMinimumSize(qMin(windowScaled(320), targetWidth),
                   qMin(windowScaled(240), targetHeight));
    const int targetX = area.left() + (area.width() - targetWidth) / 2;
    const int targetY = area.bottom() - targetHeight - 8;
    m_backdropRect = QRect(targetX, targetY, targetWidth, targetHeight);
    // 预先捕获覆盖整个动画路径的背景条带，滑动期间只裁剪内存画布。
    m_backdropCaptureRect = QRect(targetX, targetY, targetWidth,
                                  qMax(1, area.bottom() - targetY + 1));
    setGeometry(m_backdropRect);

    // 图库隐藏时捕获完整合成桌面；若原生窗口已显示，则只排除图库自身。
    QImage backdrop = captureDesktopComposite(screen, m_backdropCaptureRect, this, renderScale());
    if (!backdrop.isNull()) {
        m_backdropCanvas = std::move(backdrop);
        m_lastBackdropGeometry = QRect();
        updateBackdropFrame();
    }

    // 首次映射窗口时先放在工作区之外，避免动画开始前闪现完整面板。
    setGeometry(QRect(targetX, area.bottom() + 2,
                      targetWidth, targetHeight));
}

void WidgetLibraryDialog::resizeEvent(QResizeEvent* event)
{
    LiquidGlassWidget::resizeEvent(event);
    if (!m_tileLayout || !m_sidebar)
        return;
    const bool compact = width() < scaled(650);
    m_sidebar->setVisible(!compact);
    const int sidebarWidth = compact ? 0 : qMin(scaled(270), width() / 4);
    if (!compact)
        m_sidebar->setFixedWidth(sidebarWidth);
    const int columns = tileColumnCount();
    if (columns == m_tileColumns)
        return;
    m_tileColumns = columns;
    for (QFrame* tile : std::as_const(m_tiles))
        m_tileLayout->removeWidget(tile);
    int visibleIndex = 0;
    for (QFrame* tile : std::as_const(m_tiles)) {
        if (!tile->isVisible())
            continue;
        m_tileLayout->addWidget(tile, visibleIndex / columns, visibleIndex % columns);
        ++visibleIndex;
    }
}

void WidgetLibraryDialog::updateBackdropFrame()
{
    if (m_backdropCanvas.isNull() || m_backdropCaptureRect.isNull())
        return;
    const QRect currentGeometry = geometry();
    if (currentGeometry == m_lastBackdropGeometry)
        return;

    const qreal sx = m_backdropCanvas.width()
                     / qreal(m_backdropCaptureRect.width());
    const qreal sy = m_backdropCanvas.height()
                     / qreal(m_backdropCaptureRect.height());
    const QSize frameSize(qMax(1, qRound(currentGeometry.width() * sx)),
                          qMax(1, qRound(currentGeometry.height() * sy)));
    const QRect requested(qRound((currentGeometry.x() - m_backdropCaptureRect.x()) * sx),
                          qRound((currentGeometry.y() - m_backdropCaptureRect.y()) * sy),
                          frameSize.width(), frameSize.height());
    const QRect source = requested.intersected(m_backdropCanvas.rect());

    QImage frame(frameSize, QImage::Format_RGBA8888);
    frame.fill(Qt::transparent);
    if (!source.isEmpty()) {
        QPainter painter(&frame);
        const QRect destination(source.x() - requested.x(),
                                source.y() - requested.y(),
                                source.width(), source.height());
        painter.drawImage(destination, m_backdropCanvas, source);

        // DPI 舍入或屏幕边缘导致捕获缺列时，只延展最近边缘像素，避免露出黑色条带。
        const QRect full = frame.rect();
        if (destination.left() > full.left()) {
            painter.drawImage(QRect(full.left(), destination.top(),
                                    destination.left(), destination.height()),
                              m_backdropCanvas,
                              QRect(source.left(), source.top(), 1, source.height()));
        }
        if (destination.right() < full.right()) {
            painter.drawImage(QRect(destination.right() + 1, destination.top(),
                                    full.right() - destination.right(), destination.height()),
                              m_backdropCanvas,
                              QRect(source.right(), source.top(), 1, source.height()));
        }
        if (destination.top() > full.top()) {
            painter.drawImage(QRect(destination.left(), full.top(),
                                    destination.width(), destination.top()),
                              m_backdropCanvas,
                              QRect(source.left(), source.top(), source.width(), 1));
        }
        if (destination.bottom() < full.bottom()) {
            painter.drawImage(QRect(destination.left(), destination.bottom() + 1,
                                    destination.width(), full.bottom() - destination.bottom()),
                              m_backdropCanvas,
                              QRect(source.left(), source.bottom(), source.width(), 1));
        }
    }
    m_lastBackdropGeometry = currentGeometry;
    if (frame == m_lastBackdropImage)
        return;
    m_lastBackdropImage = frame;
    setBackgroundImage(m_lastBackdropImage);
}

void WidgetLibraryDialog::refreshBackdrop()
{
    if (!liveBackdropEnabled() || desktopDragActive() || !isVisible() || m_backdropCaptureRect.isNull()
        || m_liveCapturesInFlight >= 2)
        return;
    // 动画结束后仅采样可见面板，下一次动画之前再重建路径条带。
    const QRect area = geometry();
    const quint64 generation = m_backdropGeneration;
    ++m_liveCapturesInFlight;
    requestDesktopComposite(area, [this, generation](QImage image) {
        m_liveCapturesInFlight = qMax(0, m_liveCapturesInFlight - 1);
        if (!isVisible() || desktopDragActive() || generation != m_backdropGeneration)
            return;
        if (!image.isNull())
            ++m_liveBackdropSamples;
        if (!image.isNull() && image != m_backdropCanvas) {
            m_backdropCanvas = std::move(image);
            m_backdropRect = geometry();
            m_backdropCaptureRect = m_backdropRect;
            ++m_liveBackdropFrames;
            m_lastBackdropGeometry = QRect();
            updateBackdropFrame();
        }
        // 完成回调立即补充双请求流水线，避免等待下一次界面定时器造成捕获空闲。
        if (liveBackdropEnabled())
            refreshBackdrop();
    });
}

void WidgetLibraryDialog::hideEvent(QHideEvent* event)
{
    ++m_backdropGeneration;
    m_renderTimer.stop();
    m_liveBackdropTimer.stop();
    if (m_slideAnimation)
        m_slideAnimation->stop();
    LiquidGlassWidget::hideEvent(event);
}

void WidgetLibraryDialog::showEvent(QShowEvent* event)
{
    LiquidGlassWidget::showEvent(event);
    m_closing = false;
    ++m_backdropGeneration;
    // 属性动画已调度几何更新，无需额外定时器重复裁剪和上传纹理。
    m_renderTimer.stop();
    // 滑动期间复用预备画布，避免同步桌面捕获导致动画停顿。
    m_liveBackdropTimer.stop();
    // 图库是交互工具窗口，需要位于桌面卡片上方。
    raise();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    const int targetWidth = qMax(1, qMin(width(), area.width() - 32));
    const int targetHeight = qMax(1, qMin(height(), area.height() - 32));
    if (size() != QSize(targetWidth, targetHeight))
        resize(targetWidth, targetHeight);

    const int targetX = area.left() + (area.width() - width()) / 2;
    const int targetY = area.bottom() - height() - 8;
    const QRect endRect(targetX, targetY, width(), height());
    const QRect startRect(targetX, area.bottom() + 2, width(), height());

    if (!m_slideAnimation) {
        m_slideAnimation = new QPropertyAnimation(this, "geometry", this);
        connect(m_slideAnimation, &QPropertyAnimation::finished, this, [this]() {
            if (m_closing) {
                m_closing = false;
                m_renderTimer.stop();
                m_liveBackdropTimer.stop();
                hide();
            } else {
                // 动画结束即停止动画定时器，实时捕获仅在像素变化时请求重绘。
                m_renderTimer.stop();
                if (liveBackdropEnabled()) {
                    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
                    // 在呈现边界之间轮询，降低定时器延迟造成的丢帧；未变化帧不会重复上传。
                    m_liveBackdropTimer.setInterval(qMax(1, activeDisplayInterval() / 2));
                    m_liveBackdropTimer.start();
                    refreshBackdrop();
                } else {
                    m_liveBackdropTimer.stop();
                }
            }
        });
        connect(m_slideAnimation, &QPropertyAnimation::valueChanged,
                this, [this](const QVariant&) {
            // 几何变化后立即裁剪并调度下一帧，减少一个呈现周期的延迟。
            if (isVisible()) {
                updateBackdropFrame();
                update();
            }
        });
    }
    m_slideAnimation->stop();
    m_slideAnimation->setDuration(290);
    m_slideAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_slideAnimation->setStartValue(startRect);
    m_slideAnimation->setEndValue(endRect);
    setGeometry(startRect);
    m_slideAnimation->start();
}

void WidgetLibraryDialog::paintOverlay(QPainter& painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 内容分区只绘制分隔线，材质与边缘反光统一由玻璃渲染器提供。
    const auto panelArea = [&](QWidget* panel) {
        if (!panel || !panel->isVisible()) return QRectF();
        return QRectF(panel->mapTo(this, QPoint()), panel->size());
    };
    const QRectF sidebar = panelArea(m_sidebar);
    const QRectF footer = panelArea(m_footer);
    if (!sidebar.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 74));
        painter.drawLine(sidebar.topRight(), sidebar.bottomRight());
    }
    if (!footer.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 76));
        painter.drawLine(footer.topLeft(), footer.topRight());
    }
    painter.restore();
}

void WidgetLibraryDialog::closeGallery()
{
    if (m_slideAnimation)
        m_slideAnimation->stop();
    if (!isVisible())
        return;

    QScreen* screen = QGuiApplication::screenAt(geometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        m_renderTimer.stop();
        m_liveBackdropTimer.stop();
        hide();
        return;
    }

    const QRect area = screen->availableGeometry();
    const QRect current = geometry();
    const QRect endRect(current.x(), area.bottom() + 2,
                        current.width(), current.height());
    if (!m_slideAnimation) {
        m_renderTimer.stop();
        m_liveBackdropTimer.stop();
        hide();
        return;
    }

    m_closing = true;
    // 关闭动画也由属性变化回调裁剪，无需重复定时器。
    m_renderTimer.stop();
    // 滑动期间复用预备画布，避免同步桌面捕获导致动画停顿。
    m_liveBackdropTimer.stop();
    m_slideAnimation->setStartValue(current);
    m_slideAnimation->setEndValue(endRect);
    m_slideAnimation->start();
}

void WidgetLibraryDialog::notifyWidgetDropped(int kind, const QPoint& globalPos)
{
    emit widgetDropped(kind, globalPos);
}

void WidgetLibraryDialog::notifyWidgetDragStarted(int kind)
{
    emit widgetDragStarted(kind);
}

void WidgetLibraryDialog::notifyWidgetDragFinished(int kind)
{
    emit widgetDragFinished(kind);
}

void WidgetLibraryDialog::notifyWidgetDragPreview(int kind, const QPoint& globalPos,
                                                   bool visible)
{
    emit widgetDragPreview(kind, globalPos, visible);
}
