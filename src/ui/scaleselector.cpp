#include "ui/textcontrols.h"
#include "ui/scaleselector.h"
#include "widgets/desktopwidget.h"
#include "rendering/nativewindows.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QRegion>
#include <QShowEvent>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

class ScalePreview final : public QWidget
{
public:
    explicit ScalePreview(QWidget* owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("actualSizePreview"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(16);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qreal elapsedMs = m_frameClock.restart();
            m_displayedPercent += (m_targetPercent - m_displayedPercent)
                * (1.0 - std::exp(-elapsedMs / 45.0));
            if (qAbs(m_targetPercent - m_displayedPercent) < .1) {
                m_displayedPercent = m_targetPercent;
                m_timer.stop();
            }
            applySize();
            if (!m_timer.isActive())
                renderSettledImage();
        });
    }

    ~ScalePreview() override
    {
        NativeWindows::unregisterGlassWindow(m_registeredWindow);
    }

    void setPercent(int percent)
    {
        m_targetPercent = percent;
        if (!isVisible()) {
            m_displayedPercent = percent;
            applySize();
        } else if (!m_timer.isActive()) {
            m_frameClock.start();
            m_timer.start();
        }
    }

    QSize targetSize() const
    {
        return DesktopWidget::cardSize(DesktopWidget::CardKind::Battery, 0,
            m_targetPercent / 100.0, workArea());
    }

    void refreshPlacement() { applySize(); }

protected:
    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        // 预先生成最大尺寸的图像，连续拖动仅做栅格缩放，不触发 GL 分配或后台查询。
        const qreal dpr = devicePixelRatioF();
        m_dragImage = DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0,
            QSize(656, 656), dpr);
        applySize();
        renderSettledImage();
        m_registeredWindow = winId();
        NativeWindows::registerGlassWindow(m_registeredWindow);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect(), m_timer.isActive() ? m_dragImage : m_image);
    }

    void hideEvent(QHideEvent* event) override
    {
        m_timer.stop();
        m_displayedPercent = m_targetPercent;
        NativeWindows::unregisterGlassWindow(m_registeredWindow);
        m_registeredWindow = 0;
        QWidget::hideEvent(event);
    }

private:
    void renderSettledImage()
    {
        // 停止拖动后按最终像素重新栅格化，静止文字不继续缩放旧图像。
        m_image = DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0,
            size(), devicePixelRatioF());
        update();
    }
    QRect workArea() const
    {
        QScreen* display = parentWidget() ? parentWidget()->screen() : screen();
        return display ? display->availableGeometry() : QRect(0, 0, 1920, 1080);
    }
    void applySize()
    {
        const QRect area = workArea();
        resize(DesktopWidget::cardSize(DesktopWidget::CardKind::Battery, 0,
            m_displayedPercent / 100.0, area));
        move(area.left() + 16, qMax(area.top(), area.bottom() - height() - 15));
        // 工作区不足以并排展示时，裁去操作窗口覆盖部分；不缩小预览，也不遮挡滑块和按钮。
        const QRect controls = parentWidget()->window()->frameGeometry().translated(-pos());
        const QRegion visible = QRegion(rect()).subtracted(QRegion(controls));
        setWindowOpacity(visible.isEmpty() ? 0.0 : 1.0);
        if (mask() != visible)
            setMask(visible);
        update();
    }
    QTimer m_timer;
    QElapsedTimer m_frameClock;
    qreal m_displayedPercent = 100;
    int m_targetPercent = 100;
    QImage m_image;
    QImage m_dragImage;
    WId m_registeredWindow = 0;
};

ScaleSelector::ScaleSelector(int percent, QWidget* parent)
    : QWidget(parent), m_slider(new QSlider(Qt::Horizontal, this)),
      m_value(new AppText::Label(this)), m_dimensions(new AppText::Label(this)),
      m_preview(new ScalePreview(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto* labels = new QHBoxLayout;
    auto* title = new AppText::Label(QStringLiteral("缩放比例"), this);
    title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    m_value->setObjectName(QStringLiteral("scaleValue"));
    m_value->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600; color:#a9dcff;"));
    labels->addWidget(title);
    labels->addStretch();
    labels->addWidget(m_value);
    layout->addLayout(labels);
    m_slider->setObjectName(QStringLiteral("scaleSlider"));
    m_slider->setRange(40, 200);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(10);
    m_slider->setFixedHeight(38);
    m_slider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height:6px; border:0; border-radius:3px; background:#8294a6; }"
        "QSlider::sub-page:horizontal { border:0; border-radius:3px; background:#78c8ff; }"
        "QSlider::handle:horizontal { width:22px; margin:-8px 0; border:0; border-radius:11px; background:#ffffff; }"));
    m_slider->setValue(qBound(40, percent, 200));
    layout->addWidget(m_slider);
    auto* ends = new QHBoxLayout;
    ends->addWidget(new AppText::Label(QStringLiteral("更精巧"), this));
    ends->addStretch();
    ends->addWidget(new AppText::Label(QStringLiteral("更醒目"), this));
    layout->addLayout(ends);
    layout->addWidget(m_dimensions);
    auto* hint = new AppText::Label(QStringLiteral("屏幕左下角的浮动卡片为桌面实际尺寸。"), this);
    hint->setObjectName(QStringLiteral("scalePreviewHint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size:13px; color:#c8d9e8;"));
    layout->addWidget(hint);
    connect(m_slider, &QSlider::valueChanged, this, &ScaleSelector::updateValue);
    updateValue(m_slider->value());
}

int ScaleSelector::percent() const { return m_slider->value(); }
QSize ScaleSelector::previewSize() const { return m_preview->targetSize(); }

void ScaleSelector::updateValue(int percent)
{
    m_value->setText(QStringLiteral("%1%").arg(percent));
    m_preview->setPercent(percent);
    const QSize size = previewSize();
    m_dimensions->setText(QStringLiteral("紧凑组件 %1 × %2 · 系统显示缩放自动适配")
        .arg(size.width()).arg(size.height()));
}

void ScaleSelector::setPreviewEnabled(bool enabled)
{
    m_previewEnabled = enabled;
    m_preview->setVisible(enabled && isVisible());
}

void ScaleSelector::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    window()->installEventFilter(this);
    m_preview->setVisible(m_previewEnabled);
}

bool ScaleSelector::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == window() && m_preview->isVisible()
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        m_preview->refreshPlacement();
    return QWidget::eventFilter(watched, event);
}

void ScaleSelector::hideEvent(QHideEvent* event)
{
    m_preview->hide();
    QWidget::hideEvent(event);
}
