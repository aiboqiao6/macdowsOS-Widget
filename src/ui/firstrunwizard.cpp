#include "ui/firstrunwizard.h"
#include "widgets/batterywidget.h"
#include "app/appsettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QEasingCurve>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

FirstRunWizard::FirstRunWizard(QWidget* parent)
    : LiquidGlassWidget(parent)
{
    setWindowTitle(QStringLiteral("欢迎使用 macdowsOS Widget"));
    setPanelWindow();
    setDesktopLayerEnabled(false);
    // Read the same initial preferences as the desktop widgets without
    // creating widgets.json before the wizard is completed.
    const AppSettings initialSettings;
    // Keep the fixed wizard surface current without committing a preference
    // until the user completes the final page.
    setLiveBackdropEnabled(initialSettings.value(
        QStringLiteral("appearance/liveBackdrop")).toBool(), false);
    setGlassMargins(0);
    setGlassRadius(28);
    setRenderScale(initialSettings.value(QStringLiteral("performance/renderScale")).toFloat());
    const float blur = qBound(0, initialSettings.value(
        QStringLiteral("appearance/blurStrength")).toInt(), 100) / 100.0f;
    setBlurRadius(0.5f + blur * 9.5f);
    setBlurIterations(1 + qRound(blur * 2.0f));
    setNoiseAmount(0.008f);
    setRefractionPower(1.32f);
    QtGlassFlowScene::setGlassOpacity(
        initialSettings.value(QStringLiteral("appearance/opacity")).toFloat());

    Qt::WindowFlags flags = windowFlags();
    flags.setFlag(Qt::WindowStaysOnBottomHint, false);
    flags.setFlag(Qt::WindowStaysOnTopHint, true);
    flags.setFlag(Qt::WindowDoesNotAcceptFocus, false);
    flags.setFlag(Qt::FramelessWindowHint, true);
    setWindowFlags(flags);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setFixedSize(780, 540);

    QFont clearFont = qApp->font();
    clearFont.setFamily(BatteryWidget::pingFangFontFamily());
    clearFont.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        QFont::PreferAntialias | QFont::PreferQuality | QFont::NoSubpixelAntialias));
    clearFont.setHintingPreference(QFont::PreferFullHinting);
    setFont(clearFont);

    setStyleSheet(QStringLiteral(
        "FirstRunWizard { background:transparent; }"
        "QWidget#wizardPage, QWidget#pageHost, QLabel { background:transparent; color:#f7fbff; }"
        "QFrame#settingsPanel { background:rgba(32,54,78,90); border:1px solid rgba(180,211,237,120); border-radius:20px; }"
        "QComboBox { color:#ffffff; background:rgba(41,66,94,120); border:1px solid #8da8c0; border-radius:10px; padding:5px 14px; font-size:16px; }"
        "QComboBox::drop-down { border:0; width:28px; }"
        "QComboBox QAbstractItemView { color:#ffffff; background:#263d52; selection-background-color:#467ba7; }"
        "QCheckBox { color:#f4f8fd; font-size:15px; spacing:10px; }"
        "QCheckBox::indicator { width:18px; height:18px; }"
        "QPushButton#continueButton, QPushButton#primaryButton, QPushButton#startButton { color:#ffffff; background:#168af3; border:1px solid #a5d4ff; border-radius:12px; padding:0 28px; font-size:16px; font-weight:600; }"
        "QPushButton#continueButton:hover, QPushButton#primaryButton:hover, QPushButton#startButton:hover { background:#369cf7; }"
        "QPushButton#backButton { color:#eaf4ff; background:#304962; border:1px solid #7691ab; border-radius:12px; padding:0 24px; font-size:16px; }"
        "QPushButton#backButton:hover { background:#3b5873; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_pageHost = new QWidget(this);
    m_pageHost->setObjectName(QStringLiteral("pageHost"));
    m_pageHost->setAttribute(Qt::WA_TranslucentBackground);
    m_pageHost->installEventFilter(this);
    root->addWidget(m_pageHost);

    const auto newPage = [this](int index) {
        auto* page = new QWidget(m_pageHost);
        page->setObjectName(QStringLiteral("wizardPage"));
        page->setAttribute(Qt::WA_TranslucentBackground);
        m_pages[index] = page;
        return page;
    };
    const auto heading = [](const QString& text, QWidget* parent) {
        auto* label = new QLabel(text, parent);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("font-size:29px; font-weight:600; color:#ffffff;"));
        return label;
    };
    const auto button = [](const QString& text, const QString& name, QWidget* parent) {
        auto* result = new QPushButton(text, parent);
        result->setObjectName(name);
        result->setCursor(Qt::PointingHandCursor);
        result->setFixedHeight(48);
        result->setMinimumWidth(150);
        return result;
    };

    // Page 1: one clear welcome message and a single way forward.
    auto* welcome = newPage(0);
    auto* welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->setContentsMargins(55, 35, 55, 35);
    welcomeLayout->addStretch(2);
    auto* welcomeTitle = new QLabel(QStringLiteral("欢迎使用\nmacdowsOS Widget"), welcome);
    welcomeTitle->setObjectName(QStringLiteral("welcomeTitle"));
    welcomeTitle->setAlignment(Qt::AlignCenter);
    welcomeTitle->setStyleSheet(QStringLiteral(
        "font-size:43px; font-weight:600; color:#ffffff;"));
    welcomeLayout->addWidget(welcomeTitle);
    welcomeLayout->addSpacing(28);
    auto* welcomeNext = button(QStringLiteral("继续"), QStringLiteral("continueButton"), welcome);
    welcomeLayout->addWidget(welcomeNext, 0, Qt::AlignHCenter);
    welcomeLayout->addStretch(3);
    connect(welcomeNext, &QPushButton::clicked, this, [this]() { showPage(1); });

    // Page 2: choose the desktop widget scale directly.
    auto* scalePage = newPage(1);
    auto* scaleLayout = new QVBoxLayout(scalePage);
    scaleLayout->setContentsMargins(48, 28, 48, 28);
    scaleLayout->setSpacing(11);
    scaleLayout->addWidget(heading(QStringLiteral("选择适合的缩放"), scalePage));
    auto* scaleDescription = new QLabel(QStringLiteral("拖动滑块，选择桌面小组件的显示大小。"), scalePage);
    scaleDescription->setAlignment(Qt::AlignCenter);
    scaleDescription->setStyleSheet(QStringLiteral("font-size:15px; color:#d9e8f6;"));
    scaleLayout->addWidget(scaleDescription);

    scaleLayout->addStretch(2);

    auto* scaleRow = new QHBoxLayout;
    auto* scaleLabel = new QLabel(QStringLiteral("缩放比例"), scalePage);
    scaleLabel->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
    scaleRow->addWidget(scaleLabel);
    scaleRow->addStretch();
    m_scaleValue = new QLabel(scalePage);
    m_scaleValue->setObjectName(QStringLiteral("scaleValue"));
    m_scaleValue->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600; color:#a9dcff;"));
    scaleRow->addWidget(m_scaleValue);
    scaleLayout->addLayout(scaleRow);
    m_scaleSlider = new QSlider(Qt::Horizontal, scalePage);
    m_scaleSlider->setFixedHeight(38);
    m_scaleSlider->setRange(40, 200);
    m_scaleSlider->setSingleStep(5);
    m_scaleSlider->setPageStep(10);
    m_scaleSlider->setTickInterval(10);
    m_scaleSlider->setValue(qBound(40, qRound(initialSettings.value(
        QStringLiteral("appearance/scale")).toDouble() * 100), 200));
    scaleLayout->addWidget(m_scaleSlider);
    auto* scaleEnds = new QHBoxLayout;
    auto* small = new QLabel(QStringLiteral("更精巧"), scalePage);
    auto* large = new QLabel(QStringLiteral("更醒目"), scalePage);
    small->setStyleSheet(QStringLiteral("font-size:13px; color:#c8d9e8;"));
    large->setStyleSheet(QStringLiteral("font-size:13px; color:#c8d9e8;"));
    scaleEnds->addWidget(small);
    scaleEnds->addStretch();
    scaleEnds->addWidget(large);
    scaleLayout->addLayout(scaleEnds);
    scaleLayout->addStretch(3);
    auto* scaleFooter = new QHBoxLayout;
    auto* scaleBack = button(QStringLiteral("返回"), QStringLiteral("backButton"), scalePage);
    auto* scaleNext = button(QStringLiteral("继续"), QStringLiteral("primaryButton"), scalePage);
    scaleFooter->addWidget(scaleBack);
    scaleFooter->addStretch();
    scaleFooter->addWidget(scaleNext);
    scaleLayout->addLayout(scaleFooter);
    connect(scaleBack, &QPushButton::clicked, this, [this]() { showPage(0); });
    connect(scaleNext, &QPushButton::clicked, this, [this]() { showPage(2); });
    connect(m_scaleSlider, &QSlider::valueChanged, this, &FirstRunWizard::updateScaleValue);

    // Page 3: choose quality and live backdrop behavior.
    auto* qualityPage = newPage(2);
    auto* qualityLayout = new QVBoxLayout(qualityPage);
    qualityLayout->setContentsMargins(48, 28, 48, 28);
    qualityLayout->setSpacing(14);
    qualityLayout->addWidget(heading(QStringLiteral("调整渲染质量"), qualityPage));
    qualityLayout->addStretch();
    auto* settingsPanel = new QFrame(qualityPage);
    settingsPanel->setObjectName(QStringLiteral("settingsPanel"));
    auto* controls = new QVBoxLayout(settingsPanel);
    controls->setContentsMargins(28, 22, 28, 22);
    controls->setSpacing(9);
    auto* qualityLabel = new QLabel(QStringLiteral("渲染清晰度"), settingsPanel);
    qualityLabel->setStyleSheet(QStringLiteral("font-size:17px; font-weight:600;"));
    controls->addWidget(qualityLabel);
    m_qualityBox = new QComboBox(settingsPanel);
    m_qualityBox->setFixedHeight(48);
    m_qualityBox->addItem(QStringLiteral("节能 · 60%"), 0.60);
    m_qualityBox->addItem(QStringLiteral("均衡 · 80%"), 0.80);
    m_qualityBox->addItem(QStringLiteral("原生 · 100%"), 1.00);
    m_qualityBox->addItem(QStringLiteral("高清 · 125%"), 1.25);
    m_qualityBox->addItem(QStringLiteral("极致 · 150%"), 1.50);
    const qreal savedQuality = initialSettings.value(
        QStringLiteral("performance/renderScale")).toDouble();
    int qualityIndex = 0;
    for (int index = 0; index < m_qualityBox->count(); ++index) {
        if (qAbs(m_qualityBox->itemData(index).toDouble() - savedQuality) < 0.001)
            qualityIndex = index;
    }
    m_qualityBox->setCurrentIndex(qualityIndex);
    controls->addWidget(m_qualityBox);
    auto* qualityHint = new QLabel(QStringLiteral("画质越高，图形资源占用也会增加。"), settingsPanel);
    qualityHint->setStyleSheet(QStringLiteral("font-size:13px; color:#c9daeb;"));
    controls->addWidget(qualityHint);
    controls->addSpacing(12);
    m_liveBackdrop = new QCheckBox(QStringLiteral("实时显示小组件背后的窗口"), settingsPanel);
    m_liveBackdrop->setChecked(initialSettings.value(
        QStringLiteral("appearance/liveBackdrop")).toBool());
    connect(m_liveBackdrop, &QCheckBox::toggled, this, [this](bool enabled) {
        setLiveBackdropEnabled(enabled, false);
    });
    connect(m_qualityBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        setRenderScale(m_qualityBox->currentData().toFloat());
    });
    controls->addWidget(m_liveBackdrop);
    qualityLayout->addWidget(settingsPanel);
    qualityLayout->addStretch();
    auto* qualityFooter = new QHBoxLayout;
    auto* qualityBack = button(QStringLiteral("返回"), QStringLiteral("backButton"), qualityPage);
    auto* start = button(QStringLiteral("开始使用"), QStringLiteral("startButton"), qualityPage);
    qualityFooter->addWidget(qualityBack);
    qualityFooter->addStretch();
    qualityFooter->addWidget(start);
    qualityLayout->addLayout(qualityFooter);
    connect(qualityBack, &QPushButton::clicked, this, [this]() { showPage(1); });
    connect(start, &QPushButton::clicked, this, &FirstRunWizard::finish);

    m_pages[1]->hide();
    m_pages[2]->hide();
    rememberInterfaceMetrics();
    updateScaleValue(m_scaleSlider->value());
}

bool FirstRunWizard::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_pageHost && event->type() == QEvent::Resize && !m_transition) {
        for (QWidget* page : m_pages) {
            if (page)
                page->setGeometry(m_pageHost->rect());
        }
    }
    return LiquidGlassWidget::eventFilter(watched, event);
}

void FirstRunWizard::showEvent(QShowEvent* event)
{
    LiquidGlassWidget::showEvent(event);
    if (m_introPlayed)
        return;
    m_introPlayed = true;
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible() || m_currentPage != 0)
            return;
        auto* animation = new QPropertyAnimation(m_pages[0], "pos", this);
        animation->setDuration(420);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->setStartValue(QPoint(0, 24));
        animation->setEndValue(QPoint(0, 0));
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

void FirstRunWizard::showPage(int index)
{
    if (m_transition || index < 0 || index >= 3 || index == m_currentPage)
        return;
    QWidget* previous = m_pages[m_currentPage];
    QWidget* next = m_pages[index];
    const int direction = index > m_currentPage ? 1 : -1;
    const int distance = qMax(1, m_pageHost->width());
    next->setGeometry(QRect(direction * distance, 0, distance,
                            m_pageHost->height()));
    next->show();
    next->raise();

    auto* transition = new QParallelAnimationGroup(this);
    const auto slide = [transition](QWidget* page, const QPoint& end) {
        auto* animation = new QPropertyAnimation(page, "pos", transition);
        animation->setDuration(340);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->setStartValue(page->pos());
        animation->setEndValue(end);
        transition->addAnimation(animation);
    };
    slide(previous, QPoint(-direction * distance, 0));
    slide(next, QPoint(0, 0));
    m_transition = transition;
    connect(transition, &QParallelAnimationGroup::finished, this,
            [this, transition, previous, next, index]() {
        previous->hide();
        previous->move(0, 0);
        next->move(0, 0);
        m_currentPage = index;
        m_transition = nullptr;
        transition->deleteLater();
        applyInterfaceScale();
    });
    transition->start();
}

bool FirstRunWizard::run()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        move(area.center() - QPoint(width() / 2, height() / 2));
        // Supplying this hidden host also enables the wallpaper fallback when
        // the first desktop capture is unavailable. A solid fallback would
        // leave the glass with nothing to blur on its first visible frame.
        QImage backdrop = captureDesktopComposite(screen, geometry(), this, renderScale());
        if (backdrop.isNull()) {
            backdrop = QImage(size(), QImage::Format_RGB32);
            backdrop.fill(QColor(46, 67, 87));
        }
        setBackgroundImage(backdrop);
    }
    QEventLoop loop;
    m_loop = &loop;
    show();
    raise();
    activateWindow();
    loop.exec();
    m_loop = nullptr;
    hide();
    return m_completed;
}

void FirstRunWizard::finish()
{
    // The final slide can reach its last pixel just before the animation's
    // finished callback. A visible Start button must already be actionable.
    if (!m_pages[2]->isVisible())
        return;
    AppSettings settings;
    settings.setValue(QStringLiteral("appearance/scale"), m_scaleSlider->value() / 100.0);
    settings.setValue(QStringLiteral("performance/renderScale"), m_qualityBox->currentData());
    settings.setValue(QStringLiteral("appearance/liveBackdrop"), m_liveBackdrop->isChecked());
    settings.ensureDefaults();
    settings.sync();
    m_completed = true;
    close();
}

void FirstRunWizard::updateScaleValue(int percent)
{
    m_scaleValue->setText(QStringLiteral("%1%").arg(percent));
    m_interfaceScale = percent / 100.0;
    if (!m_transition)
        applyInterfaceScale();
}

void FirstRunWizard::rememberInterfaceMetrics()
{
    auto widgets = findChildren<QWidget*>();
    widgets.prepend(this);
    for (QWidget* widget : widgets) {
        widget->setProperty("wizardBaseStyle", widget->styleSheet());
        if (widget != this) {
            widget->setProperty("wizardBaseMinimum", widget->minimumSize());
            widget->setProperty("wizardBaseMaximum", widget->maximumSize());
        }
    }
    for (QLayout* item : findChildren<QLayout*>()) {
        const QMargins margins = item->contentsMargins();
        item->setProperty("wizardBaseMargins", QVariantList{
            margins.left(), margins.top(), margins.right(), margins.bottom()});
        item->setProperty("wizardBaseSpacing", item->spacing());
    }
}

void FirstRunWizard::applyInterfaceScale()
{
    // Scale native controls and layout metrics from their original values;
    // repeated slider moves never compound the previous scale or blur text.
    const auto scaled = [this](int value) { return qRound(value * m_interfaceScale); };
    static const QRegularExpression pixels(QStringLiteral("(-?\\d+(?:\\.\\d+)?)px"));
    auto widgets = findChildren<QWidget*>();
    widgets.prepend(this);
    for (QWidget* widget : widgets) {
        if (!widget->property("wizardBaseStyle").isValid()) continue;
        QString style = widget->property("wizardBaseStyle").toString();
        auto matches = pixels.globalMatch(style);
        QString result;
        qsizetype previous = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            result += style.mid(previous, match.capturedStart() - previous);
            result += QString::number(qRound(match.captured(1).toDouble() * m_interfaceScale))
                      + QStringLiteral("px");
            previous = match.capturedEnd();
        }
        result += style.mid(previous);
        widget->setStyleSheet(result);
        if (widget == this) continue;
        const QSize minimum = widget->property("wizardBaseMinimum").toSize();
        const QSize maximum = widget->property("wizardBaseMaximum").toSize();
        widget->setMinimumSize(scaled(minimum.width()), scaled(minimum.height()));
        widget->setMaximumSize(maximum.width() == QWIDGETSIZE_MAX ? QWIDGETSIZE_MAX : scaled(maximum.width()),
                               maximum.height() == QWIDGETSIZE_MAX ? QWIDGETSIZE_MAX : scaled(maximum.height()));
    }
    for (QLayout* item : findChildren<QLayout*>()) {
        const QVariantList margins = item->property("wizardBaseMargins").toList();
        if (margins.size() != 4) continue;
        item->setContentsMargins(scaled(margins[0].toInt()), scaled(margins[1].toInt()),
                                 scaled(margins[2].toInt()), scaled(margins[3].toInt()));
        const int spacing = item->property("wizardBaseSpacing").toInt();
        item->setSpacing(spacing < 0 ? spacing : scaled(spacing));
    }
    // QSS rejects a corner radius larger than half the handle's actual
    // extent. Round the radius first, then derive an even diameter and the
    // groove margins together; independent rounding produced square thumbs.
    const int handleRadius = qMax(1, scaled(11));
    const int grooveRadius = qMax(1, scaled(3));
    const int handleMargin = handleRadius - grooveRadius;
    m_scaleSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height:%1px; border:0; border-radius:%2px; background:#8294a6; }"
        "QSlider::sub-page:horizontal { border:0; border-radius:%2px; background:#78c8ff; }"
        "QSlider::handle:horizontal { width:%3px; margin:-%4px 0; border:0; border-radius:%5px; background:#ffffff; }")
        .arg(2 * grooveRadius).arg(grooveRadius).arg(2 * handleRadius)
        .arg(handleMargin).arg(handleRadius));

    QScreen* display = screen();
    const QRect area = display ? display->availableGeometry() : QRect();
    QSize extent(scaled(780), scaled(540));
    if (area.isValid()) extent = extent.boundedTo(area.size() - QSize(32, 32));
    const QPoint center = geometry().center();
    setFixedSize(extent);
    setGlassRadius(28 * m_interfaceScale);
    if (layout()) layout()->activate();
    for (QWidget* page : m_pages) {
        page->setGeometry(m_pageHost->rect());
        if (page->layout()) page->layout()->activate();
    }
    if (isVisible() && area.isValid()) {
        QPoint origin = center - QPoint(width() / 2, height() / 2);
        origin.setX(qBound(area.left(), origin.x(), area.right() - width() + 1));
        origin.setY(qBound(area.top(), origin.y(), area.bottom() - height() + 1));
        move(origin);
    }
    update();
}

void FirstRunWizard::closeEvent(QCloseEvent* event)
{
    LiquidGlassWidget::closeEvent(event);
    if (m_loop)
        m_loop->quit();
}

void FirstRunWizard::mousePressEvent(QMouseEvent* event)
{
    event->accept();
}

void FirstRunWizard::mouseMoveEvent(QMouseEvent* event)
{
    event->accept();
}

void FirstRunWizard::mouseReleaseEvent(QMouseEvent* event)
{
    event->accept();
}

void FirstRunWizard::paintOverlay(QPainter& painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF bounds = QRectF(rect()).adjusted(0.7, 0.7, -0.7, -0.7);
    painter.setPen(Qt::NoPen);
    QLinearGradient veil(bounds.topLeft(), bounds.bottomLeft());
    veil.setColorAt(0.0, QColor(10, 23, 38, 78));
    veil.setColorAt(0.55, QColor(10, 24, 41, 100));
    veil.setColorAt(1.0, QColor(8, 18, 33, 86));
    painter.setBrush(veil);
    painter.drawRoundedRect(bounds, 28 * m_interfaceScale, 28 * m_interfaceScale);
    QLinearGradient rim(bounds.topLeft(), bounds.bottomRight());
    rim.setColorAt(0.0, QColor(255, 255, 255, 190));
    rim.setColorAt(0.5, QColor(210, 231, 250, 58));
    rim.setColorAt(1.0, QColor(255, 255, 255, 125));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(rim), 1.2));
    painter.drawRoundedRect(bounds, 28 * m_interfaceScale, 28 * m_interfaceScale);
    painter.restore();
}
