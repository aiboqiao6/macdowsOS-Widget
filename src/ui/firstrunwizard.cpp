#include "ui/firstrunwizard.h"
#include "widgets/desktopwidget.h"
#include "app/appsettings.h"
#include "ui/scaleselector.h"

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
#include <QMouseEvent>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
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
    // 读取桌面组件相同的初始设置，向导完成之前不创建配置文件。
    const AppSettings initialSettings;
    // 向导中的画质与实时背景只预览，完成后才保存。
    setLiveBackdropEnabled(initialSettings.value(
        QStringLiteral("appearance/liveBackdrop")).toBool(), false);
    setGlassMargins(0);
    setGlassRadius(28);
    setRenderScale(initialSettings.value(QStringLiteral("performance/renderScale")).toFloat());

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
    clearFont.setFamily(DesktopWidget::pingFangFontFamily());
    DesktopWidget::applyFontSmoothing(clearFont);
    setFont(clearFont);

    setStyleSheet(QStringLiteral(
        "FirstRunWizard { background:transparent; }"
        "QWidget#wizardPage, QWidget#pageHost, QLabel { background:transparent; color:#f7fbff; }"
        "QFrame#settingsPanel { background:transparent; border:1px solid rgba(180,211,237,120); border-radius:20px; }"
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

    m_scaleSelector = new ScaleSelector(qRound(initialSettings.value(
        QStringLiteral("appearance/scale")).toDouble() * 100), scalePage);
    m_scaleSlider = m_scaleSelector->slider();
    scaleLayout->addWidget(m_scaleSelector);
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
    if (QScreen* display = screen())
        setFixedSize(QSize(780, 540).boundedTo(display->availableGeometry().size() - QSize(32, 32)));
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
    m_scaleSelector->setPreviewEnabled(false);
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
        for (QWidget* page : m_pages)
            page->setGeometry(m_pageHost->rect());
        m_scaleSelector->setPreviewEnabled(index == 1);
    });
    transition->start();
}

bool FirstRunWizard::run()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        move(area.center() - QPoint(width() / 2, height() / 2));
        // 首次捕获不可用时，隐藏宿主仍可提供壁纸回退，避免初始玻璃缺少背景细节。
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
    // 最后一页已经可见时允许立即完成，不必等待动画结束回调。
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
