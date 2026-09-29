#include "ui/widgetlibrarydialog.h"
#include "widgets/batterywidget.h"
#include "rendering/responsivelayout.h"
#include "app/appsettings.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QOperatingSystemVersion>
#include <QPainter>
#include <QPainterPath>
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

// Enlarge the gallery surface independently of its controls and previews.
// The available desktop work area still bounds the final window geometry.
constexpr int kGalleryWidth = 1320 * 3 / 2;
constexpr int kGalleryHeight = 800 * 3 / 2;
constexpr qreal kGalleryContentZoom = 1.5;

void paintNavIcon(QPainter& p, const QRectF& bounds, int category)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = qMin(bounds.width(), bounds.height()) * .1;
    const QRectF canvas = bounds.adjusted(inset, inset, -inset, -inset);
    const ResponsiveLayout::Metrics metrics(canvas);
    const QColor ink = category == 1 ? QColor(111, 232, 157, 245)
                      : category == 2 ? QColor(122, 194, 255, 245)
                      : category == 3 ? QColor(220, 229, 244, 245)
                      : category == 4 ? QColor(229, 157, 245, 245)
                                      : QColor(198, 185, 255, 245);
    p.setPen(QPen(ink, metrics.stroke(p, .075), Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (category == 1) {
        p.drawRoundedRect(metrics.rect(.16, .30, .62, .40), metrics.size(.08), metrics.size(.08));
        p.drawLine(metrics.point(.78, .42), metrics.point(.88, .42));
        p.drawLine(metrics.point(.78, .58), metrics.point(.88, .58));
    } else if (category == 2) {
        p.drawArc(metrics.rect(.20, .38, .40, .34), 35 * 16, 250 * 16);
        p.drawArc(metrics.rect(.42, .27, .38, .45), 120 * 16, 220 * 16);
        p.drawLine(metrics.point(.18, .72), metrics.point(.84, .72));
        p.drawLine(metrics.point(.28, .82), metrics.point(.72, .82));
    } else if (category == 3) {
        p.drawEllipse(metrics.rect(.15, .15, .70, .70));
        p.drawLine(metrics.point(.50, .50), metrics.point(.50, .29));
        p.drawLine(metrics.point(.50, .50), metrics.point(.67, .61));
    } else if (category == 4) {
        p.drawLine(metrics.point(.17, .27), metrics.point(.50, .43));
        p.drawLine(metrics.point(.83, .27), metrics.point(.50, .43));
        p.drawLine(metrics.point(.17, .27), metrics.point(.17, .73));
        p.drawLine(metrics.point(.83, .27), metrics.point(.83, .73));
        p.drawLine(metrics.point(.17, .73), metrics.point(.50, .57));
        p.drawLine(metrics.point(.83, .73), metrics.point(.50, .57));
    } else {
        p.drawRoundedRect(metrics.rect(.16, .16, .28, .28), 2, 2);
        p.drawRoundedRect(metrics.rect(.56, .16, .28, .28), 2, 2);
        p.drawRoundedRect(metrics.rect(.16, .56, .28, .28), 2, 2);
        p.drawRoundedRect(metrics.rect(.56, .56, .28, .28), 2, 2);
    }
    p.restore();
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

QHash<QString, QImage>& previewCache()
{
    static QHash<QString, QImage> cache;
    return cache;
}

class DragTile final : public QFrame
{
public:
    DragTile(const QString& title, const QString& subtitle, int kind,
             int variant, int category, WidgetLibraryDialog* owner, qreal scale)
        : QFrame(owner), m_title(title), m_subtitle(subtitle), m_kind(kind),
          m_variant(variant), m_category(category), m_owner(owner), m_scale(scale)
    {
        setObjectName(QStringLiteral("widgetTile"));
        setProperty("tileTitle", title);
        setFixedSize(qRound(220 * m_scale), qRound(176 * m_scale));
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::OpenHandCursor);
        m_press = QPoint(-1, -1);
        // The preview paints the real payload into an image without exposing
        // an OpenGL window. Cache it at this tile's actual screen density.
        refreshPreview();
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
            .arg(m_kind).arg(m_variant).arg(previewSize.width()).arg(previewSize.height())
            .arg(dpr, 0, 'g', 12).arg(BatteryWidget::globalFontSmoothing());
        if (!previewCache().contains(key))
            previewCache().insert(key, BatteryWidget::renderPreview(
                static_cast<BatteryWidget::CardKind>(m_kind), m_variant, previewSize, dpr));
        m_preview = previewCache().value(key);
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
        QPainter p(this);
        p.scale(m_scale, m_scale);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing,
                        BatteryWidget::globalFontSmoothing() > 0);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        // The gallery itself owns the single liquid-glass surface. A tile is
        // intentionally chrome-free so previews float directly on that pane,
        // matching the reference: no second rounded card or duplicate rim.
        const QRectF preview(8, 2, 204, 128);
        const ResponsiveLayout::Metrics previewMetrics(preview);
        if (!m_preview.isNull()) {
            const QTransform device = p.deviceTransform();
            const QPointF origin = preview.center()
                - QPointF(m_preview.deviceIndependentSize().width() * .5,
                          m_preview.deviceIndependentSize().height() * .5);
            const QPoint pixelOrigin = device.map(origin).toPoint();
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(device.inverted().map(QPointF(pixelOrigin)), m_preview);
            p.restore();
        } else {
        const auto text = [&p](const QRectF& r, const QString& value, int size,
                               const QColor& color, QFont::Weight weight = QFont::Normal,
                               Qt::Alignment align = Qt::AlignCenter) {
            QFont font = qApp->font();
            font.setPixelSize(size);
            font.setWeight(weight);
            BatteryWidget::applyFontSmoothing(font);
            p.setPen(color); p.setFont(font);
            ResponsiveLayout::drawSingleLine(p, r, align, value);
        };
        const auto ring = [&p](const QPointF& c, qreal radius, int percent) {
            const QRectF bounds(c.x() - radius, c.y() - radius,
                                radius * 2.0, radius * 2.0);
            const ResponsiveLayout::Metrics metrics(bounds);
            const qreal stroke = metrics.stroke(p, .12);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(235,244,255,55), stroke));
            p.drawEllipse(c, radius, radius);
            p.setPen(QPen(QColor(255,255,255,235), stroke));
            p.drawArc(bounds, 90 * 16, -qRound(percent * 3.6) * 16);
        };
        const auto clock = [&p](const QPointF& c, qreal radius, bool dark) {
            const QRectF bounds(c.x() - radius, c.y() - radius,
                                radius * 2.0, radius * 2.0);
            const ResponsiveLayout::Metrics metrics(bounds);
            p.setBrush(dark ? QColor(30,34,42,238) : QColor(248,249,251,242));
            p.setPen(QPen(dark ? QColor(255,255,255,55) : QColor(205,211,220,220),
                          metrics.stroke(p, .014)));
            p.drawEllipse(c,radius,radius);
            p.setPen(QPen(dark ? QColor(238,243,250,235) : QColor(56,62,72,200),
                          metrics.stroke(p, .018)));
            for(int i=0;i<12;++i){ const qreal a=i*M_PI/6-M_PI/2; p.drawLine(c+QPointF(std::cos(a),std::sin(a))*(radius*.70),c+QPointF(std::cos(a),std::sin(a))*(radius*.88)); }
            p.drawLine(c,c+QPointF(0,-radius*.45)); p.drawLine(c,c+QPointF(radius*.35,radius*.2));
            p.setBrush(QColor(247,170,119,235)); p.drawEllipse(c,2.5,2.5);
        };
        if (m_kind == 0) {
            if (m_variant == 1) { for(int i=0;i<3;++i) ring(QPointF(preview.left()+40+i*64,preview.center().y()-3),21,95-i*17); text(preview.adjusted(0,82,0,-2),QStringLiteral("已连接设备"),9,QColor(200,215,237,190)); }
            else if (m_variant == 2) {
                const QRectF list = preview.adjusted(26, 17, -26, -14);
                const auto listRow = [&p, &text](const QRectF& area, const QString& name,
                                                  const QString& value, bool mouse) {
                    const ResponsiveLayout::Metrics rowMetrics(area);
                    p.setPen(QPen(QColor(241, 247, 255, 215),
                                  rowMetrics.stroke(p, .018)));
                    p.setBrush(Qt::NoBrush);
                    if (!mouse) {
                        p.drawRoundedRect(QRectF(area.left(), area.center().y() - 4, 11, 8), 1, 1);
                        p.drawLine(QPointF(area.left() - 2, area.center().y() + 6),
                                   QPointF(area.left() + 13, area.center().y() + 6));
                    } else {
                        p.drawRoundedRect(QRectF(area.left() + 1, area.center().y() - 6, 8, 13), 3, 3);
                        p.drawLine(QPointF(area.left() + 5, area.center().y() - 4),
                                   QPointF(area.left() + 5, area.center().y() - 1));
                    }
                    text(QRectF(area.left() + 20, area.top(), area.width() - 76, area.height()), name, 8,
                         QColor(246, 250, 255, 225), QFont::DemiBold, Qt::AlignLeft | Qt::AlignVCenter);
                    text(QRectF(area.right() - 45, area.top(), 26, area.height()), value, 8,
                         QColor(246, 250, 255, 225), QFont::Normal, Qt::AlignRight | Qt::AlignVCenter);
                    p.setPen(QPen(QColor(255, 255, 255, 235),
                                  rowMetrics.stroke(p, .018)));
                    p.drawRoundedRect(QRectF(area.right() - 15, area.center().y() - 2.5, 10, 5), 1.5, 1.5);
                    p.drawLine(QPointF(area.right() - 4, area.center().y() - 1),
                               QPointF(area.right() - 2, area.center().y() - 1));
                };
                listRow(QRectF(list.left(), list.top(), list.width(), 29), QStringLiteral("本机"), QStringLiteral("—"), false);
                ResponsiveLayout::drawLine(
                    p, previewMetrics,
                    QPointF(list.left(), list.top() + list.height() * .36),
                    QPointF(list.right(), list.top() + list.height() * .36),
                    QColor(255,255,255,38), .008, Qt::FlatCap);
                listRow(QRectF(list.left(), list.top()+42, list.width(), 29), QStringLiteral("已连接设备"), QStringLiteral("—"), true);
            }
            else { p.setPen(QPen(QColor(225,235,247,195),2)); p.setBrush(QColor(225,235,247,48)); p.drawRoundedRect(QRectF(preview.left()+22,preview.top()+31,72,36),8,8); text(preview.adjusted(104,22,-8,-27),QStringLiteral("95%"),28,QColor(248,252,255,240),QFont::DemiBold); text(preview.adjusted(104,68,-8,-7),QStringLiteral("电池电量"),10,QColor(174,190,214,205)); }
        } else if (m_kind == 1) {
            text(QRectF(preview.left() + 10, preview.top() + 4, 100, 20),
                 QStringLiteral("实时地区"), 9, QColor(226, 234, 246, 220),
                 QFont::DemiBold, Qt::AlignLeft | Qt::AlignVCenter);
            text(QRectF(preview.left() + 10, preview.top() + 25, 92, 46),
                 QStringLiteral("--°"), 28, QColor(250, 252, 255, 242),
                 QFont::Normal, Qt::AlignLeft | Qt::AlignVCenter);
            p.setPen(QPen(QColor(244, 247, 251, 225), 1.4));
            p.setBrush(QColor(244, 247, 251, 225));
            p.drawEllipse(QPointF(preview.right() - 39, preview.top() + 36), 7, 7);
            p.drawRoundedRect(QRectF(preview.right() - 61, preview.top() + 43, 43, 15), 7, 7);
            if (m_variant == 1) {
                for (int i = 0; i < 6; ++i) {
                    const qreal x = preview.left() + 18 + i * 34;
                    text(QRectF(x - 13, preview.top() + 76, 26, 15),
                         QStringLiteral("时"), 7, QColor(185, 197, 217, 195));
                    p.drawEllipse(QPointF(x, preview.top() + 99), 5, 3.5);
                    text(QRectF(x - 13, preview.top() + 106, 26, 14),
                         QStringLiteral("--°"), 7, QColor(226, 233, 245, 215));
                }
            } else if (m_variant == 2) {
                ResponsiveLayout::drawLine(
                    p, previewMetrics,
                    QPointF(preview.left() + preview.width() * .05,
                            preview.top() + preview.height() * .55),
                    QPointF(preview.right() - preview.width() * .05,
                            preview.top() + preview.height() * .55),
                    QColor(255, 255, 255, 42), .008, Qt::FlatCap);
                for (int i = 0; i < 3; ++i) {
                    text(QRectF(preview.left() + 10, preview.top() + 76 + i * 17, 42, 14),
                         QStringLiteral("周%1").arg(i + 3), 7,
                         QColor(207, 216, 231, 205), QFont::DemiBold,
                         Qt::AlignLeft | Qt::AlignVCenter);
                    p.setPen(QPen(QColor(235, 240, 248, 150),
                                  previewMetrics.stroke(p, .017),
                                  Qt::SolidLine, Qt::RoundCap));
                    p.drawLine(preview.left() + 75, preview.top() + 83 + i * 17,
                               preview.right() - 34, preview.top() + 83 + i * 17);
                }
            } else {
                text(QRectF(preview.left() + 10, preview.bottom() - 30,
                            preview.width() - 20, 18), QStringLiteral("最高 --°   最低 --°"),
                     8, QColor(190, 202, 222, 200), QFont::Normal,
                     Qt::AlignLeft | Qt::AlignVCenter);
            }
        } else if (m_kind == 2) {
            if (m_variant == 2 || m_variant == 8) { text(preview.adjusted(18,24,-18,-39),QStringLiteral("05:52"),25,QColor(246,249,255,238),QFont::DemiBold); text(preview.adjusted(18,78,-18,-3),QStringLiteral("数字时钟"),9,QColor(220,229,244,220)); }
            else if (m_variant==5 || m_variant==6) { const int count=m_variant==5?4:3, cols=m_variant==5?2:3; const qreal r=m_variant==5?20:23; for(int i=0;i<count;++i) clock(QPointF(preview.left()+47+(i%cols)*63,preview.top()+36+(i/cols)*58),r,true); }
            else { clock(preview.center()+QPointF(0,-3),41,m_variant==1||m_variant==4); if(m_variant==3||m_variant==4) text(QRectF(preview.left(),preview.bottom()-21,preview.width(),18),m_variant==3?QStringLiteral("北京时间"):QStringLiteral("城市 I"),9,QColor(190,205,226,205)); }
        } else {
            text(preview.adjusted(16,12,-12,-43),m_variant==1?QStringLiteral("serendipity"):QStringLiteral("wander"),m_variant==1?24:20,QColor(250,252,255,238),QFont::DemiBold,Qt::AlignLeft|Qt::AlignVCenter); text(preview.adjusted(16,69,-12,-9),m_variant==1?QStringLiteral("n. chance discovery"):QStringLiteral("每日单词"),9,QColor(178,195,220,205),QFont::Normal,Qt::AlignLeft|Qt::AlignVCenter); p.setPen(QPen(QColor(116,174,255,210),2)); p.drawLine(preview.left()+16,preview.bottom()-12,preview.left()+88,preview.bottom()-12);
        }
        }

        p.setPen(QColor(247, 249, 253));
        QFont footerFont = qApp->font();
        footerFont.setPixelSize(14);
        footerFont.setWeight(QFont::DemiBold);
        BatteryWidget::applyFontSmoothing(footerFont);
        p.setFont(footerFont);
        ResponsiveLayout::drawSingleLine(p, QRectF(10, 134, 200, 20),
                                   Qt::AlignCenter | Qt::AlignVCenter, m_title);
        p.setPen(QColor(173, 186, 207));
        QFont subtitleFont = qApp->font();
        subtitleFont.setPixelSize(12);
        BatteryWidget::applyFontSmoothing(subtitleFont);
        p.setFont(subtitleFont);
        ResponsiveLayout::drawSingleLine(
            p, QRectF(10, 154, 200, 17),
            Qt::AlignCenter | Qt::AlignVCenter,
            QFontMetrics(p.font()).elidedText(m_subtitle, Qt::ElideRight, 192));
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
            m_owner->notifyWidgetDragFinished(m_kind * 100 + m_variant);
        m_owner->notifyWidgetDragPreview(m_kind * 100 + m_variant, QCursor::pos(), false);
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
                m_owner->notifyWidgetDropped(m_kind * 100 + m_variant, position);
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
            m_owner->notifyWidgetDragStarted(m_kind * 100 + m_variant);
        }
        const QPoint position = event->globalPosition().toPoint();
        m_owner->notifyWidgetDragPreview(m_kind * 100 + m_variant, position, !m_owner->geometry().contains(position));
        event->accept();
    }

private:
    QString m_title;
    QString m_subtitle;
    int m_kind = 0;
    int m_variant = 0;
    int m_category = -1;
    QImage m_preview;
    QPoint m_press;
    bool m_dragging = false;
    WidgetLibraryDialog* m_owner = nullptr;
    qreal m_scale = 1.0;
};

} // namespace

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
    // Use the same QtGlassFlow surface as desktop cards, but keep this
    // interactive gallery above other windows rather than on the desktop
    // layer. Its backdrop is supplied explicitly by prepareBackdrop().
    setPanelWindow();
    setDesktopLayerEnabled(false);
    setDesktopCaptureEnabled(false);
    const AppSettings settings;
    m_uiScale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    setLiveBackdropEnabled(
        settings.value(QStringLiteral("appearance/liveBackdrop"), true).toBool());
    setGlassMargins(0);
    setGlassRadius(26.0 * m_uiScale);
    setRenderScale(float(qBound<qreal>(0.5,
        settings.value(QStringLiteral("performance/renderScale"), 0.60).toDouble(), 1.5)));
    setMaterialBlurStrength(settings.value(QStringLiteral("appearance/blurStrength"), 50).toInt());
    setGlassOpacity(settings.value(QStringLiteral("appearance/opacity"), 1.0).toDouble());
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
        // Geometry and glass composition stay at 60 Hz. The cached desktop
        // canvas is cropped locally on these cheap frames; screen capture is
        // intentionally handled by the slower timer below.
        if (isVisible()) {
            updateBackdropFrame();
            update();
        }
    });
    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
    m_liveBackdropTimer.setSingleShot(false);
    // Live captures run independently of the short panel animation. The
    // interval is refreshed for the actual monitor in showEvent().
    m_liveBackdropTimer.setInterval(qMax(1, activeDisplayInterval() / 2));
    connect(&m_liveBackdropTimer, &QTimer::timeout,
            this, &WidgetLibraryDialog::refreshBackdrop);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // LiquidGlassWidget uses a compact default size for desktop cards.  The
    // gallery is a larger surface, so clear that inherited maximum constraint.
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
                                     QStringLiteral("词典") };
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
                                  this, m_uiScale * kGalleryContentZoom);
        tile->setProperty("tileKind", kind);
        tile->setProperty("tileVariant", variant);
        tile->setProperty("tileCategory", category);
        tile->setProperty("tileSearch", (title + QStringLiteral(" ") + subtitle).toLower());
        m_tiles.append(tile);
    };
    addTile(QStringLiteral("状态"), QStringLiteral("查看 Mac 和已连接的蓝牙配件的状态。"), 0, 0, 1);
    addTile(QStringLiteral("状态"), QStringLiteral("查看 Mac 和已连接的蓝牙配件的状态。"), 0, 1, 1);
    addTile(QStringLiteral("状态"), QStringLiteral("查看 Mac 和已连接的蓝牙配件的状态。"), 0, 2, 1);
    addTile(QStringLiteral("天气 · 紧凑"), QStringLiteral("温度与天气"), 1, 0, 2);
    addTile(QStringLiteral("天气 · 详细"), QStringLiteral("地点与状态"), 1, 1, 2);
    addTile(QStringLiteral("天气 · 预报"), QStringLiteral("最高与最低温度"), 1, 2, 2);
    addTile(QStringLiteral("数字时钟"), QStringLiteral("显示当前时间"), 2, 2, 3);
    addTile(QStringLiteral("时钟 I"), QStringLiteral("显示当前时间"), 2, 0, 3);
    addTile(QStringLiteral("数字时钟 · 横向"), QStringLiteral("2×1 显示当前时间"), 2, 8, 3);
    addTile(QStringLiteral("每日单词"), QStringLiteral("随机单词与释义"), 3, 0, 4);
    addTile(QStringLiteral("词典详情"), QStringLiteral("单词、音标与释义"), 3, 1, 4);
    addTile(QStringLiteral("词典卡片"), QStringLiteral("轻量词汇提醒"), 3, 2, 4);
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

    setInterfaceScale(m_uiScale);
    randomizeRecommendations();
    applyTileFilter();

}

int WidgetLibraryDialog::scaled(int value) const
{
    return qMax(1, qRound(value * m_uiScale * kGalleryContentZoom));
}

int WidgetLibraryDialog::windowScaled(int value) const
{
    return qMax(1, qRound(value * m_uiScale));
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
    if (m_scaleApplied && qFuzzyCompare(m_uiScale, newScale))
        return;
    if (m_scaleApplied)
        previewCache().clear();
    m_scaleApplied = true;
    if (m_slideAnimation && m_slideAnimation->state() == QAbstractAnimation::Running)
        m_slideAnimation->stop();
    m_uiScale = newScale;
    setGlassRadius(26.0 * m_uiScale);
    // A radius rounded above half of a fractional-scale control height makes
    // Qt's style-sheet renderer fall back to square corners.
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
        static_cast<DragTile*>(tile)->setScale(m_uiScale * kGalleryContentZoom);
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
    // Keep the landing page varied while retaining a useful mix of categories.
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
                                                                    : QStringLiteral("词典")));
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
    // Child tiles handle mouse capture; the gallery background stays fixed.
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
    // The gallery is a transient chooser. Clicking another application (or
    // another top-level window) deactivates it; close immediately instead of
    // leaving a topmost panel stranded on screen.
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
    // Capture one fixed strip covering every visible position of the panel.
    // During the slide, changing geometry is serviced by a memory crop rather
    // than another full QScreen::grabWindow call on every rendered frame.
    m_backdropCaptureRect = QRect(targetX, targetY, targetWidth,
                                  qMax(1, area.bottom() - targetY + 1));
    setGeometry(m_backdropRect);

    // The gallery is hidden at this point. Capture the already-composited
    // desktop, including application windows and every visible desktop card;
    // only the gallery itself is excluded when it is already native-visible.
    QImage backdrop = captureDesktopComposite(screen, m_backdropCaptureRect, this, renderScale());
    if (!backdrop.isNull()) {
        m_backdropCanvas = std::move(backdrop);
        m_lastBackdropGeometry = QRect();
        updateBackdropFrame();
    }

    // The native HWND must be mapped for the first time while it is already
    // below the work area. Moving it from the final on-screen rectangle in
    // showEvent() allows Windows to present one full-size frame before the
    // slide animation starts, which is perceived as a bright flash.
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

        // A DPI rounding difference or a screen capture clipped at an edge
        // must never expose the transparent clear color as a black strip.
        // Extend only the missing edge pixels from the nearest captured row or
        // column; the correctly aligned interior is left untouched.
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
    // The prepared strip is needed while the gallery slides in, but once the
    // panel is settled it needlessly captures a much taller desktop region on
    // every live update. Sample only the visible panel; the next animation is
    // preceded by prepareBackdrop()/setInterfaceScale(), which rebuilds the
    // strip cache when the geometry changes.
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
        // Refill the two-frame pipeline immediately. Waiting for the next GUI
        // timer tick here lets queued paints leave the capture worker idle.
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
    // QPropertyAnimation already schedules geometry repaints. A second 60 Hz
    // timer here duplicated updateBackdropFrame()/update() work and caused
    // animation frames to queue behind texture uploads.
    m_renderTimer.stop();
    // Reuse the prepared backdrop canvas during the slide; grabWindow is a
    // blocking desktop capture and causes visible animation stalls.
    m_liveBackdropTimer.stop();
    // The reusable base places desktop cards at the bottom of the z-order.
    // The gallery is an interactive tool window and must be above the cards.
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
                // Stop animation wakeups once settled. The worker keeps the
                // real backdrop live and only changed pixels trigger paint.
                m_renderTimer.stop();
                if (liveBackdropEnabled()) {
                    m_liveBackdropTimer.setTimerType(Qt::PreciseTimer);
                    // Poll between presentation boundaries so a delayed Qt
                    // timer wake-up does not skip the following DWM frame.
                    // WGC still supplies at most one new frame per display
                    // refresh and unchanged frames are not uploaded.
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
            // QPropertyAnimation advances geometry before the next timer
            // wake-up. Crop and schedule the new frame immediately to remove
            // one 16 ms presentation cycle of input-to-glass latency.
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
    // Child frames paint after the GL surface and can leak rectangular
    // backgrounds beyond its alpha silhouette. Paint their tint here with
    // the same rounded outline; their controls remain ordinary widgets.
    QPainterPath silhouette;
    silhouette.addRoundedRect(QRectF(rect()), 26.0 * m_uiScale, 26.0 * m_uiScale);
    const auto panelTint = [&](QWidget* panel, const QColor& color) {
        if (!panel || !panel->isVisible()) return QRectF();
        const QRectF area(panel->mapTo(this, QPoint()), panel->size());
        QPainterPath panelPath;
        panelPath.addRect(area);
        painter.fillPath(silhouette.intersected(panelPath), color);
        return area;
    };
    const QRectF sidebar = panelTint(m_sidebar, QColor(238, 245, 251, 36));
    const QRectF footer = panelTint(m_footer, QColor(235, 243, 251, 42));
    if (!sidebar.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 74));
        painter.drawLine(sidebar.topRight(), sidebar.bottomRight());
    }
    if (!footer.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 76));
        painter.drawLine(footer.topLeft(), footer.topRight());
    }
    const qreal edge = qMax<qreal>(0.5, 0.7 * m_uiScale);
    const QRectF bounds = QRectF(rect()).adjusted(edge, edge, -edge, -edge);
    QLinearGradient rim(bounds.topLeft(), bounds.bottomRight());
    rim.setColorAt(0.0, QColor(255, 255, 255, 172));
    rim.setColorAt(.24, QColor(255, 255, 255, 60));
    rim.setColorAt(.62, QColor(200, 220, 242, 38));
    rim.setColorAt(1.0, QColor(255, 255, 255, 130));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(rim), 1.2 * m_uiScale));
    painter.drawRoundedRect(bounds, windowScaled(26), windowScaled(26));
    painter.setPen(QPen(QColor(255, 255, 255, 55), m_uiScale));
    painter.drawLine(QPointF(windowScaled(28), 1.4 * m_uiScale),
                     QPointF(width() - windowScaled(28), 1.4 * m_uiScale));
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
    // The animation's valueChanged handler updates the crop and schedules the
    // repaint. Avoid a duplicate timer while the panel slides away.
    m_renderTimer.stop();
    // Reuse the prepared backdrop canvas during the slide; grabWindow is a
    // blocking desktop capture and causes visible animation stalls.
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
