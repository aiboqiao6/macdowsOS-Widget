#include "widgetlibrarydialog.h"

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

class DragTile final : public QFrame
{
public:
    DragTile(const QString& title, const QString& subtitle, int kind,
             WidgetLibraryDialog* owner)
        : QFrame(owner), m_title(title), m_subtitle(subtitle), m_kind(kind), m_owner(owner)
    {
        setObjectName(QStringLiteral("widgetTile"));
        setProperty("tileTitle", title);
        setFixedSize(184, 148);
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::OpenHandCursor);
        m_press = QPoint(-1, -1);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        const bool hovered = underMouse();
        // Match the actual desktop cards: translucent neutral glass, one
        // restrained rim, and no opaque black preview panel inside it.
        QLinearGradient surface(card.topLeft(), card.bottomRight());
        surface.setColorAt(0.0, hovered ? QColor(255, 255, 255, 54)
                                        : QColor(255, 255, 255, 28));
        surface.setColorAt(0.52, QColor(130, 164, 210, 18));
        surface.setColorAt(1.0, QColor(8, 16, 28, 58));
        p.setPen(QPen(hovered ? QColor(230, 243, 255, 160)
                              : QColor(255, 255, 255, 82), 1));
        p.setBrush(surface);
        p.drawRoundedRect(card, 18, 18);

        const QRectF preview(12, 12, width() - 24, 83);

        if (m_kind == 0) {
            const QPointF c(preview.left() + 40, preview.center().y());
            const qreal radius = 24.0;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(225, 235, 249, 48), 5));
            p.drawEllipse(c, radius, radius);
            p.setPen(QPen(QColor(239, 244, 255, 218), 5));
            p.drawArc(QRectF(c.x() - radius, c.y() - radius, radius * 2, radius * 2),
                      90 * 16, -342 * 16);
            p.setPen(QPen(QColor(230, 237, 248, 190), 1.5));
            p.drawRoundedRect(QRectF(c.x() - 10, c.y() - 7, 20, 14), 2, 2);
            p.drawLine(QPointF(c.x() - 14, c.y() + 10), QPointF(c.x() + 14, c.y() + 10));
            p.setPen(QColor(246, 250, 255, 230));
            p.setFont(QFont(QStringLiteral("Segoe UI"), 19, QFont::DemiBold));
            p.drawText(preview.adjusted(75, 8, -4, -8), Qt::AlignVCenter,
                       QStringLiteral("95%"));
        } else if (m_kind == 1) {
            p.setPen(QColor(250, 252, 255));
            p.setFont(QFont(QStringLiteral("Segoe UI"), 25, QFont::DemiBold));
            p.drawText(preview.adjusted(10, 2, 0, -12), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("24°"));
            p.setPen(QColor(165, 183, 207));
            p.setFont(QFont(QStringLiteral("Segoe UI"), 10));
            p.drawText(preview.adjusted(12, 50, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("晴朗  ·  最高 28°"));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 209, 83));
            p.drawEllipse(QPointF(preview.right() - 28, preview.top() + 28), 10, 10);
        } else if (m_kind == 2) {
            const QPointF c(preview.center());
            p.setBrush(QColor(246, 248, 250));
            p.setPen(QPen(QColor(215, 220, 228), 2));
            p.drawEllipse(c, 29, 29);
            p.setPen(QPen(QColor(33, 39, 48), 2));
            p.drawLine(c, c + QPointF(0, -18));
            p.drawLine(c, c + QPointF(15, 11));
            p.setBrush(QColor(33, 39, 48));
            p.drawEllipse(c, 3, 3);
        } else {
            p.setPen(QColor(255, 255, 255, 235));
            p.setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::DemiBold));
            p.drawText(preview.adjusted(14, 8, -10, -8), Qt::AlignLeft | Qt::AlignTop,
                       QStringLiteral("random word"));
            p.setPen(QColor(160, 177, 202));
            p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
            p.drawText(preview.adjusted(14, 35, -10, -8), Qt::AlignLeft | Qt::AlignTop,
                       QStringLiteral("online dictionary"));
            p.setPen(QColor(114, 173, 255));
            p.drawLine(preview.left() + 14, preview.bottom() - 16,
                       preview.left() + 86, preview.bottom() - 16);
        }

        p.setPen(QColor(247, 249, 253));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 13, QFont::DemiBold));
        p.drawText(QRectF(14, 101, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
                   m_title);
        p.setPen(QColor(173, 186, 207));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        p.drawText(QRectF(14, 121, width() - 28, 17), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(m_subtitle, Qt::ElideRight, width() - 28));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_press = event->pos();
            setCursor(Qt::ClosedHandCursor);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        setCursor(Qt::OpenHandCursor);
        QFrame::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_press.x() < 0 || !(event->buttons() & Qt::LeftButton)) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        if ((event->pos() - m_press).manhattanLength() < QApplication::startDragDistance())
            return;

        QDrag drag(this);
        auto* mime = new QMimeData;
        mime->setData(QStringLiteral("application/x-macdows-widget-kind"),
                      QByteArray::number(m_kind));
        drag.setMimeData(mime);
        QPixmap dragPixmap(132, 96);
        dragPixmap.fill(Qt::transparent);
        QPainter painter(&dragPixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(17, 23, 32, 235));
        painter.setPen(QPen(QColor(255, 255, 255, 95), 1));
        painter.drawRoundedRect(dragPixmap.rect().adjusted(1, 1, -1, -1), 16, 16);
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 14, QFont::DemiBold));
        painter.drawText(dragPixmap.rect(), Qt::AlignCenter, m_title);
        drag.setPixmap(dragPixmap);
        drag.setHotSpot(QPoint(dragPixmap.width() / 2, dragPixmap.height() / 2));

        QTimer previewTimer;
        previewTimer.setTimerType(Qt::PreciseTimer);
        previewTimer.setInterval(16);
        const auto updatePreview = [this]() {
            const QPoint cursor = QCursor::pos();
            const bool outsideLibrary = !m_owner->geometry().contains(cursor);
            m_owner->notifyWidgetDragPreview(m_kind, cursor, outsideLibrary);
        };
        QObject::connect(&previewTimer, &QTimer::timeout, m_owner, updatePreview);
        updatePreview();
        previewTimer.start();
        drag.exec(Qt::CopyAction);
        previewTimer.stop();
        m_owner->notifyWidgetDragPreview(m_kind, QCursor::pos(), false);

        const QPoint globalPos = QCursor::pos();
        if (!m_owner->geometry().contains(globalPos))
            m_owner->notifyWidgetDropped(m_kind, globalPos);
        m_press = QPoint(-1, -1);
    }

private:
    QString m_title;
    QString m_subtitle;
    int m_kind = 0;
    QPoint m_press;
    WidgetLibraryDialog* m_owner = nullptr;
};

} // namespace

WidgetLibraryDialog::WidgetLibraryDialog(QWidget* parent)
    : LiquidGlassWidget(parent)
{
    setWindowTitle(QStringLiteral("小组件"));
    // Use the same QtGlassFlow surface as desktop cards, but keep this
    // interactive gallery above other windows rather than on the desktop
    // layer. Its backdrop is supplied explicitly by prepareBackdrop().
    setDesktopLayerEnabled(false);
    setDesktopCaptureEnabled(false);
    setGlassMargins(0);
    setGlassRadius(26.0);
    setBlurRadius(6.0f);
    setBlurIterations(3);
    setNoiseAmount(0.008f);
    setRefractionPower(1.32f);
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
        // QtGlassFlow's idle coalescing is ideal for desktop cards, but this
        // gallery is an animated surface: keep its shader/composite at 60 Hz
        // while it is open so refraction and the live slide feel continuous.
        if (isVisible())
            update();
    });
    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
    m_liveBackdropTimer.setInterval(16); // animation-time capture/render at 60 Hz
    connect(&m_liveBackdropTimer, &QTimer::timeout,
            this, &WidgetLibraryDialog::refreshBackdrop);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // LiquidGlassWidget uses a compact default size for desktop cards.  The
    // gallery is a larger surface, so clear that inherited maximum constraint.
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setMinimumSize(900, 560);
    resize(1180, 720);
    setStyleSheet(QStringLiteral(
        "WidgetLibraryDialog { background:transparent; color:#f7f9fd; border:0; }"
        "QFrame#sidebar { background:rgba(238,245,251,36); border-right:1px solid rgba(255,255,255,74); }"
        "QFrame#footer { background:rgba(235,243,251,42); border-top:1px solid rgba(255,255,255,76); }"
        "QLabel#sectionTitle { color:#f8fbff; font-size:18px; font-weight:600; }"
        "QLabel#footerHint { color:rgba(246,250,255,225); font-size:13px; }"
        "QPushButton#navButton, QPushButton#navSelected { text-align:left; border:0; border-radius:12px; padding-left:10px; color:rgba(246,249,253,230); font-size:13px; }"
        "QPushButton#navButton:hover { background:rgba(255,255,255,48); }"
        "QPushButton#navSelected { background:rgba(255,255,255,104); color:#ffffff; font-weight:600; }"
        "QPushButton#doneButton { background:#1684f7; border:1px solid rgba(255,255,255,90); border-radius:18px; color:white; font-size:14px; font-weight:600; padding:0 23px; }"
        "QPushButton#doneButton:hover { background:#3195fb; }"
        "QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget { background:transparent; border:0; }"
        "QScrollBar:vertical { width:7px; background:transparent; }"
        "QScrollBar::handle:vertical { background:rgba(255,255,255,96); border-radius:3px; min-height:35px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"));

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
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(270);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(18, 22, 16, 18);
    sideLayout->setSpacing(5);

    // The library currently exposes one category only. Keep the label text
    // but remove the unused navigation icons and placeholder categories so
    // the left rail does not compete with the actual widget previews.
    auto* allWidgets = new QPushButton(QStringLiteral("所有小组件"), sidebar);
    allWidgets->setObjectName(QStringLiteral("navSelected"));
    allWidgets->setCursor(Qt::PointingHandCursor);
    allWidgets->setFlat(true);
    allWidgets->setFixedHeight(39);
    allWidgets->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    allWidgets->setProperty("navText", QStringLiteral("所有小组件"));
    allWidgets->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; border:0; border-radius:12px; "
        "padding-left:12px; color:#ffffff; background:rgba(255,255,255,104); "
        "font-size:13px; font-weight:600; }"));
    sideLayout->addWidget(allWidgets);
    sideLayout->addStretch(1);

    auto* content = new QWidget(body);
    content->setAttribute(Qt::WA_TranslucentBackground);
    content->setAttribute(Qt::WA_NoSystemBackground);
    content->setAutoFillBackground(false);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(30, 23, 25, 12);
    contentLayout->setSpacing(12);
    auto* sectionTitle = new QLabel(QStringLiteral("建议"), content);
    sectionTitle->setObjectName(QStringLiteral("sectionTitle"));
    contentLayout->addWidget(sectionTitle);

    auto* scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAttribute(Qt::WA_TranslucentBackground);
    auto* scrollHost = new QWidget(scroll);
    scrollHost->setAttribute(Qt::WA_TranslucentBackground);
    auto* tileLayout = new QGridLayout(scrollHost);
    tileLayout->setContentsMargins(0, 0, 0, 12);
    tileLayout->setHorizontalSpacing(14);
    tileLayout->setVerticalSpacing(14);
    const auto addTile = [this, tileLayout](const QString& title, const QString& subtitle,
                                            int kind, int row, int col) {
        auto* tile = new DragTile(title, subtitle, kind, this);
        m_tiles.append(tile);
        tileLayout->addWidget(tile, row, col);
    };
    addTile(QStringLiteral("电池"), QStringLiteral("电量"), 0, 0, 0);
    addTile(QStringLiteral("天气"), QStringLiteral("温度与天气"), 1, 0, 1);
    addTile(QStringLiteral("时钟"), QStringLiteral("当前时间"), 2, 0, 2);
    addTile(QStringLiteral("词典"), QStringLiteral("每日单词"), 3, 0, 3);
    tileLayout->setColumnStretch(4, 1);
    scroll->setWidget(scrollHost);
    contentLayout->addWidget(scroll, 1);
    bodyLayout->addWidget(sidebar);
    bodyLayout->addWidget(content, 1);
    root->addWidget(body, 1);

    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("footer"));
    footer->setFixedHeight(70);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(24, 0, 23, 0);
    auto* footerHint = new QLabel(QStringLiteral("将小组件拖放到桌面…"), footer);
    footerHint->setObjectName(QStringLiteral("footerHint"));
    footerLayout->addWidget(footerHint);
    footerLayout->addStretch(1);
    auto* done = new QPushButton(QStringLiteral("完成"), footer);
    done->setObjectName(QStringLiteral("doneButton"));
    done->setFixedHeight(36);
    footerLayout->addWidget(done);
    root->addWidget(footer);
    connect(done, &QPushButton::clicked, this, &WidgetLibraryDialog::closeGallery);

}

void WidgetLibraryDialog::mousePressEvent(QMouseEvent* event)
{
    // The gallery itself is not a draggable desktop card. Child tiles handle
    // their own real QDrag operation; clicks on empty/background areas should
    // never move the whole window.
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

void WidgetLibraryDialog::prepareBackdrop()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    const int targetWidth = qMin(width(), qMax(1, area.width() - 32));
    const int targetHeight = qMin(height(), qMax(1, area.height() - 32));
    const int targetX = area.left() + (area.width() - targetWidth) / 2;
    const int targetY = area.bottom() - targetHeight - 8;
    m_backdropRect = QRect(targetX, targetY, targetWidth, targetHeight);
    setGeometry(m_backdropRect);

    // The gallery is hidden at this point. grabWindow(0) therefore captures
    // the already-composited desktop, including editor/terminal windows, but
    // cannot capture the gallery itself. QtGlassFlow performs the blur and
    // refraction from this image using the same shader/FBO path as cards.
    const QPixmap backdrop = screen->grabWindow(0, targetX, targetY,
                                                targetWidth, targetHeight);
    if (!backdrop.isNull()) {
        m_lastBackdropImage = backdrop.toImage();
        setBackgroundImage(m_lastBackdropImage);
    }
}

void WidgetLibraryDialog::enableCaptureExclusion()
{
    m_captureExcluded = false;
#ifdef Q_OS_WIN
    // WDA_EXCLUDEFROMCAPTURE (0x11) keeps QScreen::grabWindow focused on the
    // already-composited windows behind this panel instead of feeding the
    // panel's own previous frame back into the liquid-glass texture.
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window)
        m_captureExcluded = SetWindowDisplayAffinity(window, 0x11) != FALSE;
#endif
}

void WidgetLibraryDialog::refreshBackdrop()
{
    if (!isVisible() || !m_captureExcluded || m_backdropRect.isNull())
        return;
    // Follow the slide geometry while the panel is entering/leaving. Once a
    // frame is completely below the work area, keep sampling the last valid
    // rectangle so the animation never flashes an empty black texture.
    QRect captureRect = geometry();
    QScreen* screen = QGuiApplication::screenAt(captureRect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    if (!captureRect.intersects(screen->geometry()))
        captureRect = m_backdropRect;

    const QPixmap backdrop = screen->grabWindow(0, captureRect.x(), captureRect.y(),
                                                captureRect.width(), captureRect.height());
    if (!backdrop.isNull()) {
        const QImage image = backdrop.toImage();
        // QScreen::grabWindow is still needed to detect external window
        // changes, but avoid invalidating the OpenGL blur cache when the
        // captured pixels are identical. This keeps the coarse idle sampler
        // cheap while preserving live updates when something behind the panel
        // really changes.
        if (image == m_lastBackdropImage)
            return;
        m_lastBackdropImage = image;
        setBackgroundImage(m_lastBackdropImage);
    }
}

void WidgetLibraryDialog::showEvent(QShowEvent* event)
{
    LiquidGlassWidget::showEvent(event);
    m_closing = false;
    enableCaptureExclusion();
    // Opening is a geometry animation. Keep both the scene and the backdrop
    // sampler at 60 Hz only for this short interval so the glass tracks every
    // animation frame without leaving a permanent high-frequency timer.
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    m_renderTimer.setInterval(16);
    m_renderTimer.start();
    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
    m_liveBackdropTimer.setInterval(16);
    m_liveBackdropTimer.start();
    // The reusable base places desktop cards at the bottom of the z-order.
    // The gallery is an interactive tool window and must be above the cards.
    raise();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    const int targetWidth = qMin(width(), area.width() - 32);
    const int targetHeight = qMin(height(), area.height() - 32);
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
                // Once the panel has settled, its shader is time-invariant and
                // its backdrop is static. Keep only a coarse sampler for
                // external desktop changes; repaint requests from search,
                // hover and tile interaction remain event-driven.
                m_renderTimer.stop();
                m_liveBackdropTimer.setTimerType(Qt::CoarseTimer);
                m_liveBackdropTimer.setInterval(1000);
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
    const QRectF bounds = QRectF(rect()).adjusted(.7, .7, -.7, -.7);
    QLinearGradient rim(bounds.topLeft(), bounds.bottomRight());
    rim.setColorAt(0.0, QColor(255, 255, 255, 172));
    rim.setColorAt(.24, QColor(255, 255, 255, 60));
    rim.setColorAt(.62, QColor(200, 220, 242, 38));
    rim.setColorAt(1.0, QColor(255, 255, 255, 130));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(rim), 1.2));
    painter.drawRoundedRect(bounds, 26, 26);
    painter.setPen(QPen(QColor(255, 255, 255, 55), 1.0));
    painter.drawLine(QPointF(28, 1.4), QPointF(width() - 28, 1.4));
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
    // Closing must be rendered as a live glass surface, not as a cached final
    // frame. Temporarily restore the animation cadence before moving the
    // window so both geometry and the sampled backdrop advance together.
    m_renderTimer.setTimerType(Qt::PreciseTimer);
    m_renderTimer.setInterval(16);
    m_renderTimer.start();
    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
    m_liveBackdropTimer.setInterval(16);
    m_liveBackdropTimer.start();
    m_slideAnimation->setStartValue(current);
    m_slideAnimation->setEndValue(endRect);
    m_slideAnimation->start();
}

void WidgetLibraryDialog::notifyWidgetDropped(int kind, const QPoint& globalPos)
{
    emit widgetDropped(kind, globalPos);
}

void WidgetLibraryDialog::notifyWidgetDragPreview(int kind, const QPoint& globalPos,
                                                   bool visible)
{
    emit widgetDragPreview(kind, globalPos, visible);
}
