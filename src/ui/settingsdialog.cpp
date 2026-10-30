#include "ui/textcontrols.h"
#include "settingsdialog.h"
#include "settingsappearance.h"
#include "settingscontrolpaint.h"
#include "settingsbackdrop.h"
#include "scaleselector.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include "config.h"
#include "network/updatechecker.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QAbstractSpinBox>
#include <QSlider>
#include <QAction>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMutex>
#include <QWaitCondition>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <cmath>
#include <atomic>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// QWidget's translucent backing store is presented by DWM, not an OpenGL swap
// chain. Wait on that compositor off the UI thread to pace background requests.
class SettingsFrameClock final : public QThread
{
public:
    SettingsFrameClock(QObject* parent, std::function<void(bool)> tick)
        : QThread(parent), m_tick(std::move(tick)) {}
    ~SettingsFrameClock() override { stopClock(); }
    void setRefreshRate(qreal rate)
    {
        if (!std::isfinite(rate) || rate <= 1) rate = 60;
        const qint64 interval = qRound64(1000000. / rate);
        if (m_intervalUs.exchange(interval) != interval) m_wake.wakeAll();
    }
    void startClock()
    {
        if (isRunning()) return;
        ++m_epoch;
        m_pending = false;
        start();
    }
    void setInteracting(bool interacting)
    {
        if (m_interacting.exchange(interacting) != interacting) m_wake.wakeAll();
    }
    void stopClock()
    {
        ++m_epoch;
        requestInterruption();
        {
            QMutexLocker lock(&m_waitMutex);
            m_wake.wakeAll();
        }
        wait();
        m_pending = false;
    }
protected:
    void run() override
    {
        const quint64 epoch = m_epoch;
        QElapsedTimer published;
        published.start();
        bool first = true;
        while (!isInterruptionRequested()) {
            const qint64 interval = m_intervalUs;
            const qint64 budget = qMax(interval, m_interacting ? qint64(16667) : qint64(33333));
            // Wait until one compositor interval before the next useful tick.
            // High-refresh monitors need no intervening DwmFlush calls.
            const qint64 remaining = budget - published.nsecsElapsed() / 1000 - interval;
            if (!first && remaining > 1000) {
                sleepFor(remaining);
                continue;
            }
            QElapsedTimer waited;
            waited.start();
            bool synchronized = false;
#ifdef Q_OS_WIN
            synchronized = SUCCEEDED(DwmFlush());
#endif
            // DWM can return immediately when there are no pending presents (or
            // under remote desktop). Avoid a busy loop without blocking input.
            const qint64 elapsed = waited.nsecsElapsed() / 1000;
            if (elapsed < interval / 2) sleepFor(interval - elapsed);
            if (isInterruptionRequested()) break;
            // Keep DWM pacing, but wake the UI at 30 Hz when stationary and at
            // up to 60 Hz while dragging, regardless of widget frame preferences.
            if (!first && published.nsecsElapsed() / 1000 + 1000 < budget) continue;
            if (!m_pending.exchange(true)) {
                first = false;
                published.restart();
                QMetaObject::invokeMethod(this, [this, epoch, synchronized] {
                    if (epoch != m_epoch) return;
                    m_tick(synchronized);
                    m_pending = false;
                }, Qt::QueuedConnection);
            }
        }
    }
private:
    void sleepFor(qint64 microseconds)
    {
        QMutexLocker lock(&m_waitMutex);
        if (!isInterruptionRequested())
            m_wake.wait(&m_waitMutex, ulong((microseconds + 999) / 1000));
    }
    QMutex m_waitMutex;
    QWaitCondition m_wake;
    std::function<void(bool)> m_tick;
    std::atomic<qint64> m_intervalUs{16667};
    std::atomic<quint64> m_epoch{0};
    std::atomic<bool> m_pending{false};
    std::atomic<bool> m_interacting{false};
};

namespace {
using namespace SettingsAppearance;
constexpr qreal kSettingsScaleBoost = 1.3;
const QStringList kTitles = {QStringLiteral("通用"), QStringLiteral("外观与显示"),
    QStringLiteral("液态玻璃"), QStringLiteral("天气"), QStringLiteral("自定义小组件"), QStringLiteral("关于")};
const QStringList kCaptions = {QStringLiteral("设置小组件的桌面交互与登录启动方式。"), QStringLiteral("选择小组件的外观、显示大小与渲染效果。"),
    QStringLiteral("调整玻璃表面的透明度、折射与模糊效果。"), QStringLiteral("选择天气小组件显示的地区。"),
    QStringLiteral("创建和管理自己的桌面小组件。"), QStringLiteral("查看应用信息、版本号与软件更新。")};
QIcon pageIcon(int index, const QWidget* theme)
{
    const char* roles[] = {"iconGeneral", "iconAppearance", "iconMaterial", "iconWeather", "iconWidgets", "iconAbout"};
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(color(theme, roles[index]));
    p.drawRoundedRect(QRectF(1, 1, 30, 30), 7, 7);
    p.setPen(QPen(Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (index) {
    case SettingsDialog::General:
        p.drawEllipse(QPointF(16, 16), 7, 7);
        p.drawEllipse(QPointF(16, 16), 3, 3);
        for (int i = 0; i < 8; ++i) {
            p.save(); p.translate(16, 16); p.rotate(i * 45);
            p.drawLine(QPointF(0, -7), QPointF(0, -10)); p.restore();
        }
        break;
    case SettingsDialog::Appearance:
        p.drawEllipse(QRectF(6, 6, 20, 20));
        p.setBrush(Qt::white); p.drawChord(QRectF(6, 6, 20, 20), 90 * 16, 180 * 16);
        break;
    case SettingsDialog::Material:
        p.drawRoundedRect(QRectF(7, 11, 14, 14), 4, 4);
        p.setBrush(QColor(255, 255, 255, 65));
        p.drawRoundedRect(QRectF(12, 6, 14, 14), 4, 4);
        break;
    case SettingsDialog::Weather:
        p.drawEllipse(QPointF(16, 16), 5, 5);
        for (int i = 0; i < 8; ++i) {
            p.save(); p.translate(16, 16); p.rotate(i * 45);
            p.drawLine(QPointF(0, -8), QPointF(0, -10)); p.restore();
        }
        break;
    case SettingsDialog::Widgets:
        for (int y : {7, 18}) for (int x : {7, 18})
            p.drawRoundedRect(QRectF(x, y, 7, 7), 2, 2);
        break;
    case SettingsDialog::About:
        p.drawEllipse(QRectF(6, 6, 20, 20));
        p.drawPoint(QPointF(16, 11));
        p.drawLine(QPointF(16, 15), QPointF(16, 22));
        break;
    }
    return QIcon(pixmap);
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* label = new AppText::Label(text, parent);
    label->setProperty("secondary", true);
    label->setWordWrap(true);
    return label;
}

QIcon searchIcon(const QColor& ink)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ink, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(QRectF(5, 4, 17, 17));
    p.drawLine(QPointF(20, 19), QPointF(27, 26));
    return QIcon(pixmap);
}
}

SettingsDialog::SettingsDialog(bool darkMode)
    : QDialog(nullptr, Qt::Window | Qt::FramelessWindowHint), m_blur(new SettingsBackdrop(this))
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("macdowsOS Widget 设置"));
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground);
    AppFonts::useInterfaceText(this);
    SettingsAppearance::load(this);
    setProperty("settingsDark", darkMode);
    QFont textFont = font();
    const qreal points = textFont.pointSizeF() > 0 ? textFont.pointSizeF()
        : QFontInfo(textFont).pixelSize() * 72. / logicalDpiY();
    // Enlarge settings at every saved scale without changing desktop widget scale.
    textFont.setPointSizeF(points * number(this, "fontScale") * kSettingsScaleBoost);
    setFont(textFont);
    m_baseFont = textFont;
    m_blur->setObjectName(QStringLiteral("settingsSidebarBlur"));
    m_blur->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_blur->setFocusPolicy(Qt::NoFocus);
    static_cast<SettingsBackdrop*>(m_blur)->presented = [this] {
        setProperty("settingsBackdropFrames", property("settingsBackdropFrames").toULongLong() + 1);
    };

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_sidebar = new QWidget(this);
    m_sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    m_sidebar->installEventFilter(this);
    auto* side = new QVBoxLayout(m_sidebar);
    margins(side, "sidebarInset", "", "sidebarInset", "sidebarInset", "navigationGap");
    auto* trafficBar = new QWidget(m_sidebar);
    SettingsAppearance::height(trafficBar, "toolbarHeight");
    trafficBar->setProperty("settingsDragRegion", true);
    trafficBar->installEventFilter(this);
    auto* traffic = new QHBoxLayout(trafficBar);
    margins(traffic, "trafficInset", "", "", "", "trafficGap");
    const QStringList names = {QStringLiteral("关闭"), QStringLiteral("最小化"), QStringLiteral("缩放窗口")};
    for (int i = 0; i < 3; ++i) {
        auto* button = new SettingsAppearance::PaintButton(this, trafficBar);
        button->setObjectName(QStringList{QStringLiteral("settingsClose"),
            QStringLiteral("settingsMinimize"), QStringLiteral("settingsZoom")}.at(i));
        square(button, "trafficDiameter");
        button->setProperty("traffic", true);
        button->setProperty("settingsPaint", QStringLiteral("traffic"));
        button->setProperty("trafficIndex", i);
        button->setAutoDefault(false);
        button->setAccessibleName(names[i]);
        button->setToolTip(names[i]);
        traffic->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] {
            if (i == 0) reject();
            else if (i == 1) showMinimized();
            else isMaximized() ? showNormal() : showMaximized();
        });
    }
    traffic->addStretch();
    side->addWidget(trafficBar);
    auto* searchSpace = new QWidget(m_sidebar);
    SettingsAppearance::height(searchSpace, "searchTop");
    side->addWidget(searchSpace);
    auto* searchField = new QFrame(m_sidebar);
    searchField->setObjectName(QStringLiteral("settingsSearchField"));
    SettingsAppearance::height(searchField, "searchHeight");
    auto* searchLayout = new QHBoxLayout(searchField);
    margins(searchLayout, "searchInset", "", "searchInset", "");
    m_search = new AppText::LineEdit(searchField);
    m_search->setObjectName(QStringLiteral("settingsSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索"));
    m_search->setAccessibleName(QStringLiteral("搜索设置"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(searchIcon(color(this, "searchForeground")), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    searchLayout->addWidget(m_search);
    side->addWidget(searchField);
    auto* navigationSpace = new QWidget(m_sidebar);
    SettingsAppearance::height(navigationSpace, "navigationTop");
    side->addWidget(navigationSpace);

    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("settingsContent"));
    auto* right = new QVBoxLayout(content);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    auto* toolbar = new QWidget(content);
    toolbar->setObjectName(QStringLiteral("settingsToolbar"));
    SettingsAppearance::height(toolbar, "toolbarHeight");
    toolbar->setProperty("settingsDragRegion", true);
    toolbar->installEventFilter(this);
    auto* header = new QHBoxLayout(toolbar);
    margins(header, "toolbarInset", "", "pageInset", "", "toolbarGap");
    auto* history = new QFrame(toolbar);
    history->setObjectName(QStringLiteral("settingsHistoryControl"));
    SettingsAppearance::height(history, "historyHeight");
    auto* historyLayout = new QHBoxLayout(history);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(0);
    const auto historyButton = [&](const QString& name, const QString& title, int direction) {
        auto* button = new SettingsAppearance::PaintButton(this, history);
        button->setObjectName(name);
        button->setProperty("settingsPaint", QStringLiteral("history"));
        button->setProperty("historyDirection", direction);
        button->setProperty("themeWidth", QStringLiteral("historyButtonWidth"));
        button->setAccessibleName(title);
        button->setToolTip(title);
        button->setAutoDefault(false);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        return button;
    };
    m_back = historyButton(QStringLiteral("settingsBack"), QStringLiteral("后退"), -1);
    m_forward = historyButton(QStringLiteral("settingsForward"), QStringLiteral("前进"), 1);
    auto* historyDivider = new QFrame(history);
    historyDivider->setObjectName(QStringLiteral("settingsHistorySeparator"));
    SettingsAppearance::height(historyDivider, "historySeparatorHeight");
    historyDivider->setProperty("themeWidth", QStringLiteral("stroke"));
    historyLayout->addWidget(m_back);
    historyLayout->addWidget(historyDivider, 0, Qt::AlignVCenter);
    historyLayout->addWidget(m_forward);
    header->addWidget(history);
    connect(m_back, &QPushButton::clicked, this, [this] { navigateHistory(-1); });
    connect(m_forward, &QPushButton::clicked, this, [this] { navigateHistory(1); });
    for (bool forward : {false, true}) {
        auto* action = new QAction(this);
        action->setShortcut(forward ? QKeySequence::Forward : QKeySequence::Back);
        addAction(action);
        connect(action, &QAction::triggered, forward ? m_forward : m_back, &QPushButton::click);
    }
    m_title = new AppText::Label(toolbar);
    m_title->setObjectName(QStringLiteral("settingsTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(m_title, 1);
    right->addWidget(toolbar);
    m_stack = new QStackedWidget(content);
    m_stack->setObjectName(QStringLiteral("settingsPages"));
    right->addWidget(m_stack, 1);
    for (int i = General; i <= About; ++i) {
        if (i == Weather || i == About) {
            auto* gap = new QWidget(m_sidebar);
            gap->setObjectName(QStringLiteral("settingsNavigationSection"));
            gap->setProperty("settingsNavigationSectionStart", i);
            SettingsAppearance::height(gap, "navigationSectionGap");
            side->addWidget(gap);
        }
        auto* button = new SettingsAppearance::PaintButton(this, m_sidebar);
        button->setIcon(pageIcon(i, this));
        button->setText(kTitles[i]);
        button->setObjectName(QStringLiteral("settingsNav%1").arg(i));
        button->setProperty("navigation", true);
        button->setProperty("settingsPaint", QStringLiteral("navigation"));
        button->setCheckable(true);
        button->setAutoDefault(false);
        SettingsAppearance::height(button, "navigationHeight");
        side->addWidget(button);
        m_navigation.append(button);
        connect(button, &QPushButton::clicked, this, [this, i] { selectPage(i); });
        auto* scroll = new QScrollArea(m_stack);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->viewport()->setObjectName(QStringLiteral("settingsPageViewport"));
        scroll->viewport()->setAutoFillBackground(false);
        auto* body = new QWidget(scroll);
        auto* page = new QVBoxLayout(body);
        margins(page, "pageInset", "pageTop", "pageInset", "pageInset", "groupGap");
        page->addStretch();
        m_pages.append(page);
        scroll->setWidget(body);
        m_stack->addWidget(scroll);
    }
    m_noResults = caption(QStringLiteral("没有匹配的设置"), m_sidebar);
    m_noResults->setContentsMargins(px(this, "navigationInset"), px(this, "controlInset"), 0, 0);
    m_noResults->hide();
    side->addWidget(m_noResults);
    side->addStretch();
    root->addWidget(m_sidebar);
    root->addWidget(content, 1);
    buildAboutPage();
    connect(m_search, &QLineEdit::textChanged, this, &SettingsDialog::filterPages);
    m_frameClock = new SettingsFrameClock(this, [this](bool synchronized) {
        if (!isVisible() || isMinimized()) return;
        setProperty("settingsVSyncActive", synchronized);
        setProperty("settingsVSyncTicks", property("settingsVSyncTicks").toULongLong() + 1);
        refreshBackdrop();
    });
    setDarkAppearance(darkMode);
    selectPage(General);
    const QSize available = screen()->availableGeometry().size()
        - QSize(px(this, "screenInset"), px(this, "screenInset")) * 2;
    setMinimumSize(QSize(px(this, "minimumWidth"), px(this, "minimumHeight")).boundedTo(available));
    setMaximumSize(available);
    m_unscaledWindowSize = QSize(px(this, "windowWidth"), px(this, "windowHeight"));
    resize(m_unscaledWindowSize.toSize().boundedTo(available));
    setProperty("settingsReady", true);
    setInterfaceScale(m_interfaceScale);
}

SettingsDialog::~SettingsDialog()
{
    m_frameClock->stopClock();
    NativeWindows::unregisterGlassWindow(m_registeredWindow);
}

QVBoxLayout* SettingsDialog::addGroup(Page page, const QString& title)
{
    m_layoutDirty = true;
    auto* layout = m_pages[page];
    const bool firstGroup = layout->count() == 1;
    if (!firstGroup && !title.isEmpty()) {
        auto* label = caption(title, this);
        label->setProperty("groupTitle", true);
        label->setContentsMargins(px(this, "groupInset"), px(this, "groupTitleTop"),
            0, px(this, "groupTitleBottom"));
        layout->insertWidget(layout->count() - 1, label);
    }
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("settingsGroup"));
    auto* group = new QVBoxLayout(frame);
    margins(group, "groupInset", "stroke", "groupInset", "stroke");
    group->setContentsMargins(px(this, "groupInset"), px(this, "stroke"),
        px(this, "groupInset"), px(this, "stroke"));
    group->setSpacing(0);
    if (firstGroup) {
        auto* overview = new QWidget(frame);
        auto* overviewLayout = new QHBoxLayout(overview);
        margins(overviewLayout, "", "overviewPadding", "", "overviewPadding", "rowGap");
        auto* icon = new AppText::Label(overview);
        icon->setProperty("settingsOverviewIcon", int(page));
        square(icon, "overviewIcon");
        overviewLayout->addWidget(icon, 0, Qt::AlignTop);
        auto* copy = new QVBoxLayout;
        margins(copy, "", "", "", "", "copyGap");
        auto* heading = new AppText::Label(kTitles[page], overview);
        heading->setProperty("settingsOverviewTitle", true);
        copy->addWidget(heading);
        copy->addWidget(caption(kCaptions[page], overview));
        overviewLayout->addLayout(copy, 1);
        group->addWidget(overview);
    }
    layout->insertWidget(layout->count() - 1, frame);
    return group;
}

void SettingsDialog::addRow(QVBoxLayout* group, const QString& label, QWidget* control, const QString& description)
{
    m_layoutDirty = true;
    if (control) m_rowControls.append(control);
    if (group->count()) {
        auto* line = new QFrame(group->parentWidget());
        line->setObjectName(QStringLiteral("settingsSeparator"));
        SettingsAppearance::height(line, "stroke");
        line->setFixedHeight(px(this, "stroke"));
        group->addWidget(line);
    }
    auto* row = new QWidget(group->parentWidget());
    auto* layout = new QHBoxLayout(row);
    margins(layout, "", "rowPadding", "", "rowPadding", "rowGap");
    layout->setContentsMargins(0, px(this, "rowPadding"), 0, px(this, "rowPadding"));
    layout->setSpacing(px(this, "rowGap"));
    auto* copy = new QVBoxLayout;
    margins(copy, "", "", "", "", "copyGap");
    copy->setSpacing(px(this, "copyGap"));
    auto* title = new AppText::Label(label, row);
    title->setProperty("settingsRowTitle", true);
    title->setWordWrap(true);
    copy->addWidget(title);
    if (!description.isEmpty()) copy->addWidget(caption(description, row));
    layout->addLayout(copy, 1);
    if (control) {
        control->setAccessibleName(label);
        control->setProperty("settingsRowControl", true);
        if (auto* value = qobject_cast<QLabel*>(control)) {
            value->setWordWrap(true);
            value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (auto* numberControl = qobject_cast<QAbstractSpinBox*>(control)) {
            SettingsAppearance::height(numberControl, "selectorDiameter");
            new SettingsAppearance::StepperIndicator(this, numberControl);
            new SettingsAppearance::FocusIndicator(this, numberControl);
            numberControl->setProperty("settingsStepper", true);
            numberControl->installEventFilter(this);
            if (auto* editor = numberControl->findChild<QLineEdit*>()) editor->setAlignment(Qt::AlignRight);
        }
        if (auto* toggle = qobject_cast<QCheckBox*>(control)) {
            // The native indicator is on the left, while our switch is drawn on
            // the right. Give both the same bounds, including text-bearing boxes.
            toggle->setAccessibleDescription(toggle->text());
            toggle->setText({});
            SettingsAppearance::height(toggle, "controlHeight");
            toggle->setProperty("themeWidth", QStringLiteral("toggleWidth"));
        }
        if (qobject_cast<QComboBox*>(control) || qobject_cast<QCheckBox*>(control)) {
            control->setProperty("settingsPaint", qobject_cast<QComboBox*>(control)
                ? QStringLiteral("selector") : QStringLiteral("toggle"));
            control->installEventFilter(this);
        }
        layout->addWidget(control);
        title->setBuddy(control);
    }
    group->addWidget(row);
}

void SettingsDialog::setDarkAppearance(bool dark)
{
    if (property("settingsReady").toBool() && m_dark == dark) return;
    m_dark = dark;
    m_layoutDirty = true;
    setProperty("settingsDark", dark);
    m_search->actions().first()->setIcon(searchIcon(color(this, "searchForeground")));
    QPalette searchPalette = m_search->palette();
    searchPalette.setColor(QPalette::PlaceholderText, color(this, "searchForeground"));
    m_search->setPalette(searchPalette);
    refreshLayout();
    refreshNavigationInk();
    update();
}

void SettingsDialog::setInterfaceScale(qreal scale)
{
    if (!std::isfinite(scale)) return;
    m_interfaceScale = qBound(qreal(.4), scale, qreal(2));
    setProperty("settingsInterfaceScale", m_interfaceScale);
    updateInterfaceScale();
}

void SettingsDialog::updateInterfaceScale()
{
    if (m_updatingScale || !m_stack) return;
    m_updatingScale = true;
    const qreal baseUnit = m_baseFont.pointSizeF() * logicalDpiY() / 72.;
    const int inset = qRound(baseUnit * number(this, "screenInset"));
    const QRect available = screen()->availableGeometry().adjusted(inset, inset, -inset, -inset);
    const qreal fit = qMin(available.width() / (baseUnit * number(this, "minimumWidth")),
        available.height() / (baseUnit * number(this, "minimumHeight")));
    const qreal effective = qMin(m_interfaceScale, qMax(qreal(.1), fit));
    if (property("settingsEffectiveScale").isValid() && qFuzzyCompare(effective, m_effectiveScale)
        && maximumSize() == available.size() && logicalDpiY() == m_layoutDpi && devicePixelRatioF() == m_layoutDpr) {
        m_updatingScale = false;
        return;
    }
    // Keep the unclipped size so enlarging past the screen edge and returning
    // to the previous scale restores the window instead of gradually shrinking it.
    const QSize requested = (m_unscaledWindowSize * effective).toSize();
    const QPoint center = frameGeometry().center();
    m_effectiveScale = effective;
    setProperty("settingsEffectiveScale", effective);
    QFont scaledFont = m_baseFont;
    scaledFont.setPointSizeF(m_baseFont.pointSizeF() * effective);
    setProperty("settingsLayingOut", true);
    setFont(scaledFont);
    setMinimumSize(QSize(px(this, "minimumWidth"), px(this, "minimumHeight")).boundedTo(available.size()));
    setMaximumSize(available.size());
    const QSize target = requested.expandedTo(minimumSize()).boundedTo(maximumSize());
    resize(target);
    if (!isMaximized()) {
        move(qBound(available.left(), center.x() - width() / 2, available.right() - width() + 1),
             qBound(available.top(), center.y() - height() / 2, available.bottom() - height() + 1));
    }
    setProperty("settingsLayingOut", false);
    m_layoutDirty = true;
    refreshLayout();
    m_updatingScale = false;
}

void SettingsDialog::selectPage(int index)
{
    if (index < 0 || index >= m_pages.size()) return;
    if (m_historyPosition < 0 || m_pageHistory[m_historyPosition] != index) {
        m_pageHistory.resize(m_historyPosition + 1);
        m_pageHistory.append(index);
        m_historyPosition = m_pageHistory.size() - 1;
    }
    showPage(index);
}

void SettingsDialog::navigateHistory(int offset)
{
    const int position = m_historyPosition + offset;
    if (position < 0 || position >= m_pageHistory.size()) return;
    m_search->clear();
    m_historyPosition = position;
    showPage(m_pageHistory[position]);
}

void SettingsDialog::showPage(int index)
{
    m_stack->setCurrentIndex(index);
    m_title->setText(kTitles[index]);
    for (int i = 0; i < m_navigation.size(); ++i) m_navigation[i]->setChecked(i == index);
    m_back->setEnabled(m_historyPosition > 0);
    m_forward->setEnabled(m_historyPosition + 1 < m_pageHistory.size());
}

void SettingsDialog::filterPages(const QString& query)
{
    int first = -1;
    for (int i = 0; i < m_navigation.size(); ++i) {
        QString text = kTitles[i] + kCaptions[i];
        for (auto* label : m_pages[i]->parentWidget()->findChildren<QLabel*>()) text += label->text();
        const bool matches = text.contains(query.trimmed(), Qt::CaseInsensitive);
        m_navigation[i]->setVisible(matches);
        if (matches && first < 0) first = i;
    }
    for (auto* gap : m_sidebar->findChildren<QWidget*>(QStringLiteral("settingsNavigationSection"))) {
        const int begin = gap->property("settingsNavigationSectionStart").toInt();
        const int end = begin == Weather ? About : int(m_navigation.size());
        bool before = false, after = false;
        for (int i = 0; i < begin; ++i) before |= !m_navigation[i]->isHidden();
        for (int i = begin; i < end; ++i) after |= !m_navigation[i]->isHidden();
        gap->setVisible(before && after);
    }
    m_noResults->setVisible(first < 0);
    if (first >= 0 && m_navigation[m_stack->currentIndex()]->isHidden()) selectPage(first);
}

void SettingsDialog::buildAboutPage()
{
    auto* version = addGroup(About, QStringLiteral("应用信息"));
    addRow(version, QStringLiteral("名称"), new AppText::Label(QStringLiteral("macdowsOS Widget"), this));
    auto* release = new AppText::Label(QString::fromLatin1(AppConfig::Version), this);
    release->setObjectName(QStringLiteral("aboutVersion"));
    addRow(version, QStringLiteral("版本"), release);
    auto* build = new AppText::Label(QString::fromLatin1(AppConfig::BuildId), this);
    build->setObjectName(QStringLiteral("aboutBuild"));
    addRow(version, QStringLiteral("版本号"), build);
    buildUpdateSection();
}

void SettingsDialog::buildUpdateSection()
{
    auto* checker = new UpdateChecker(this);
    auto* group = addGroup(About, QStringLiteral("软件更新"));
    auto* check = new QPushButton(QStringLiteral("检查更新"), this);
    check->setObjectName(QStringLiteral("checkForUpdates"));
    check->setAutoDefault(false);
    addRow(group, QStringLiteral("获取最新版本"), check, QStringLiteral("包括正式版与 Beta 测试版。"));
    auto* status = new AppText::Label(QStringLiteral("点击“检查更新”以连接 GitHub。"), this);
    status->setObjectName(QStringLiteral("updateStatus"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setContentsMargins(0, px(this, "controlInset"), 0, px(this, "rowPadding"));
    group->addWidget(status);
    auto* notes = caption({}, this);
    notes->setTextFormat(Qt::PlainText);
    notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notes->setContentsMargins(0, 0, 0, px(this, "rowPadding"));
    notes->hide();
    group->addWidget(notes);
    auto* download = new QPushButton(QStringLiteral("前往下载 ↗"), this);
    download->setObjectName(QStringLiteral("downloadUpdate"));
    download->setAutoDefault(false);
    download->hide();
    group->addWidget(download, 0, Qt::AlignRight);
    connect(check, &QPushButton::clicked, checker, &UpdateChecker::check);
    connect(download, &QPushButton::clicked, this, [checker] { QDesktopServices::openUrl(checker->releaseUrl()); });
    connect(checker, &UpdateChecker::changed, this, [=] {
        const bool checking = checker->state() == UpdateChecker::State::Checking;
        const bool available = checker->state() == UpdateChecker::State::Available;
        check->setEnabled(!checking);
        check->setText(checking ? QStringLiteral("正在检查…") : QStringLiteral("重新检查"));
        status->setText(checker->message() + (available ? QStringLiteral("\n%1\n版本号 %2")
            .arg(checker->releaseName(), checker->releaseTag().left(8)) : QString()));
        notes->setText(checker->releaseNotes());
        notes->setVisible(available && !checker->releaseNotes().isEmpty());
        download->setVisible(available);
        if (available) showUpdateDialog(checker);
    });
}

void SettingsDialog::showUpdateDialog(UpdateChecker* checker)
{
    auto* popup = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);
    popup->setObjectName(QStringLiteral("updateAvailableDialog"));
    popup->setWindowTitle(QStringLiteral("有可用更新"));
    popup->setWindowModality(Qt::WindowModal);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_TranslucentBackground);
    auto* root = new QVBoxLayout(popup);
    root->setContentsMargins(0, 0, 0, 0);
    auto* card = new QFrame(popup);
    card->setObjectName(QStringLiteral("updateCard"));
    root->addWidget(card);
    auto* layout = new QVBoxLayout(card);
    margins(layout, "pageInset", "pageInset", "pageInset", "pageInset", "controlGap");
    auto* header = new QHBoxLayout;
    auto* icon = new AppText::Label(card);
    icon->setPixmap(pageIcon(About, this).pixmap(px(this, "avatarSize"), px(this, "avatarSize")));
    header->addWidget(icon);
    auto* heading = new AppText::Label(QStringLiteral("有可用更新"), card);
    heading->setProperty("heading", true);
    header->addWidget(heading, 1);
    layout->addLayout(header);
    auto* title = new AppText::Label(checker->releaseName(), card);
    title->setObjectName(QStringLiteral("updateReleaseTitle"));
    title->setTextFormat(Qt::PlainText);
    title->setProperty("heading", true);
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(caption(QStringLiteral("%1 → %2")
        .arg(QString::fromLatin1(AppConfig::BuildId).left(8), checker->releaseTag().left(8)), card));
    auto* logHeading = new AppText::Label(QStringLiteral("更新日志"), card);
    logHeading->setProperty("heading", true);
    layout->addWidget(logHeading);
    auto* log = new QTextBrowser(card);
    log->setObjectName(QStringLiteral("updateReleaseNotes"));
    log->setOpenLinks(false);
    log->setFrameShape(QFrame::NoFrame);
    log->setStyleSheet(QStringLiteral("QTextBrowser {background:transparent; border:0;}"));
    log->setPlainText(checker->releaseNotes().isEmpty() ? QStringLiteral("此版本暂无更新日志。") : checker->releaseNotes());
    layout->addWidget(log, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* later = new QPushButton(QStringLiteral("稍后"), card);
    later->setAutoDefault(false);
    buttons->addWidget(later);
    auto* download = new QPushButton(QStringLiteral("前往下载 ↗"), card);
    download->setObjectName(QStringLiteral("downloadUpdate"));
    download->setDefault(true);
    buttons->addWidget(download);
    layout->addLayout(buttons);
    connect(later, &QPushButton::clicked, popup, &QDialog::reject);
    const QUrl url = checker->releaseUrl();
    connect(download, &QPushButton::clicked, popup, [popup, url] {
        QDesktopServices::openUrl(url);
        popup->accept();
    });
    SettingsAppearance::applyMetrics(this);
    popup->resize(QSize(px(this, "popupWidth"), px(this, "popupHeight"))
        .boundedTo(size() - QSize(px(this, "pageInset"), px(this, "pageInset")) * 2));
    popup->show();
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    // Shared controls keep their own presentation elsewhere; the settings host supplies its theme.
    for (auto* widget : findChildren<QWidget*>()) {
        if (auto* selector = dynamic_cast<ScaleSelector*>(widget)) {
            for (auto* child : selector->findChildren<QWidget*>()) child->setStyleSheet({});
            selector->findChild<QLabel*>(QStringLiteral("scaleValue"))->setProperty("heading", true);
            SettingsAppearance::height(selector->slider(), "sliderHeight");
            if (!selector->slider()->findChild<QWidget*>(QStringLiteral("settingsFocusIndicator")))
                new SettingsAppearance::FocusIndicator(this, selector->slider());
            margins(selector->layout(), "", "", "", "", "controlGap");
        }
    }
    refreshLayout();
    QDialog::showEvent(event);
    ++m_generation;
    m_registeredWindow = winId();
    if (!m_screenConnection)
        m_screenConnection = connect(windowHandle(), &QWindow::screenChanged, this, [this] { updateInterfaceScale(); });
    NativeWindows::registerGlassWindow(m_registeredWindow);
    m_frameClock->setRefreshRate(screen()->refreshRate());
    m_frameClock->startClock();
    refreshBackdrop();
}

void SettingsDialog::refreshLayout()
{
    if (!m_sidebar || !m_stack) return;
    if (property("settingsLayingOut").toBool()) return;
    setProperty("settingsLayingOut", true);
    const bool metricsChanged = m_layoutDirty || font() != m_layoutFont
        || logicalDpiY() != m_layoutDpi || devicePixelRatioF() != m_layoutDpr;
    if (metricsChanged) {
        const auto widgets = findChildren<QWidget*>();
        for (auto* widget : widgets) {
            if (!widget->property("settingsBodyFont").toBool()) {
                widget->setProperty("settingsBodyFont", true);
            }
        }
        // QSS polishing emits FontChange too. Keep stylesheet and metric updates
        // inside the same guard so theme/font changes cannot recursively shrink
        // controls while Qt is still resolving their inherited fonts.
        setStyleSheet(SettingsAppearance::stylesheet(this));
        AppFonts::useInterfaceText(this);
        SettingsAppearance::applyMetrics(this);
        for (auto* button : m_navigation)
            button->setIconSize(QSize(px(this, "navigationIcon"), px(this, "navigationIcon")));
        for (auto* widget : findChildren<QWidget*>()) {
            auto* label = qobject_cast<QLabel*>(widget);
            if (!label) continue;
            if (label->property("groupTitle").toBool())
                label->setContentsMargins(px(this, "groupInset"), px(this, "groupTitleTop"), 0, px(this, "groupTitleBottom"));
            if (label->property("settingsOverviewIcon").isValid())
                label->setPixmap(pageIcon(label->property("settingsOverviewIcon").toInt(), this)
                    .pixmap(QSize(px(this, "overviewIcon"), px(this, "overviewIcon")), devicePixelRatioF()));
        }
        m_layoutFont = font();
        m_layoutDpi = logicalDpiY();
        m_layoutDpr = devicePixelRatioF();
        m_layoutDirty = false;
    }
    if (metricsChanged || m_layoutWidth != width()) {
        m_layoutWidth = width();
        const int ceiling = qMax(0, qMin(px(this, "sidebarMaximum"), width() - px(this, "contentMinimum")));
        m_sidebar->setFixedWidth(qBound(qMin(px(this, "sidebarMinimum"), ceiling),
            qRound(width() * number(this, "sidebarRatio")), ceiling));
        const int maximum = qMax(px(this, "selectorDiameter"), qMin(px(this, "controlMaximum"),
            qRound((width() - m_sidebar->width() - px(this, "pageInset") * 2) * number(this, "controlRatio"))));
        if (metricsChanged || maximum != m_controlMaximum) {
            m_controlMaximum = maximum;
            for (const auto& guarded : m_rowControls) {
                auto* control = guarded.data();
                if (!control) continue;
                control->setMaximumWidth(maximum);
                if (auto* value = qobject_cast<QLabel*>(control))
                    value->setMinimumWidth(qMin(maximum,
                        value->fontMetrics().horizontalAdvance(value->text()) + px(this, "controlGap")));
                if (auto* indicator = control->findChild<QWidget*>(QStringLiteral("settingsStepperIndicator"))) {
                    const int d = px(this, "selectorDiameter");
                    indicator->setGeometry(control->width() - d, (control->height() - d) / 2, d, d);
                    indicator->raise();
                }
                if (auto* focus = control->findChild<QWidget*>(QStringLiteral("settingsFocusIndicator"))) {
                    focus->setGeometry(control->rect());
                    focus->raise();
                }
                if (auto* combo = qobject_cast<QComboBox*>(control)) {
                    int widest = 0;
                    for (int i = 0; i < combo->count(); ++i)
                        widest = qMax(widest, combo->fontMetrics().horizontalAdvance(combo->itemText(i)));
                    combo->setMinimumWidth(qMin(maximum, widest + px(this, "selectorDiameter") + px(this, "controlGap")));
                }
            }
        }
    }
    m_blur->setGeometry(m_sidebar->geometry());
    static_cast<SettingsBackdrop*>(m_blur)->setCornerRadius(px(this, "windowRadius"));
    setProperty("settingsLayingOut", false);
}

void SettingsDialog::refreshNavigationInk()
{
    // Unselected navigation uses the theme's fixed ink; the backdrop never recolors it.
    const QColor ink = color(this, m_dark ? "navigationLight" : "navigationDark");
    for (auto* button : m_navigation) {
        if (button->property("settingsNavigationInk").value<QColor>() != ink) {
            button->setProperty("settingsNavigationInk", ink);
            button->update();
        }
    }
}

void SettingsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange && property("settingsReady").toBool()
        && !property("settingsLayingOut").toBool()) {
        if (!m_updatingScale) {
            m_baseFont = font();
            const qreal points = font().pointSizeF() > 0 ? font().pointSizeF()
                : QFontInfo(font()).pixelSize() * 72. / logicalDpiY();
            m_baseFont.setPointSizeF(points / m_effectiveScale);
        }
        refreshLayout();
    }
    if (event->type() == QEvent::WindowStateChange && m_frameClock && m_registeredWindow) {
        if (isMinimized()) {
            m_frameClock->stopClock();
            ++m_generation;
            m_capturePending = false;
            m_captureCadence.invalidate();
            m_lastBackdrop = {};
            static_cast<SettingsBackdrop*>(m_blur)->clearPending();
        } else if (isVisible()) {
            m_frameClock->startClock();
            refreshBackdrop();
        }
    }
}

void SettingsDialog::hideEvent(QHideEvent* event)
{
    m_dragging = false;
    m_frameClock->setInteracting(false);
    if (auto* grabber = QWidget::mouseGrabber(); grabber && grabber->window() == this)
        grabber->releaseMouse();
    m_frameClock->stopClock();
    setProperty("settingsVSyncActive", false);
    m_lastBackdrop = {};
    static_cast<SettingsBackdrop*>(m_blur)->clearPending();
    ++m_generation;
    m_capturePending = false;
    m_captureCadence.invalidate();
    NativeWindows::unregisterGlassWindow(m_registeredWindow);
    m_registeredWindow = 0;
    QDialog::hideEvent(event);
}

void SettingsDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (event->spontaneous() && !m_updatingScale && !isMaximized() && property("settingsReady").toBool())
        m_unscaledWindowSize = QSizeF(size()) / m_effectiveScale;
    refreshLayout();
    m_blur->setGeometry(m_sidebar->geometry());
}

void SettingsDialog::moveEvent(QMoveEvent* event)
{
    QDialog::moveEvent(event);
    if (logicalDpiY() != m_layoutDpi || devicePixelRatioF() != m_layoutDpr) refreshLayout();
    if (m_frameClock && screen()) m_frameClock->setRefreshRate(screen()->refreshRate());
    refreshBackdrop();
}

void SettingsDialog::refreshBackdrop()
{
    if (!isVisible() || isMinimized() || !m_registeredWindow || m_capturePending) return;
    if (static_cast<SettingsBackdrop*>(m_blur)->hasPendingBackdrop()) return;
    // Mouse moves can arrive much faster than the display clock. They must not
    // turn capture into a free-running producer whose frames are never shown.
    if (m_captureCadence.isValid() && m_captureCadence.elapsed() < (m_dragging ? 16 : 33)) return;
    // Only the sidebar is translucent. Opaque content never needs capture or
    // glass composition; retain movement padding for in-flight drag captures.
    QRect area(m_blur->mapToGlobal(QPoint()), m_blur->size());
    if (m_dragging) {
        const qreal overscan = number(this, "sidebarCaptureOverscan");
        const int x = qRound(width() * overscan), y = qRound(height() * overscan);
        area.adjust(-x, -y, x, y);
    }
    m_capturePending = true;
    m_captureCadence.start();
    const quint64 generation = m_generation;
    const qreal captureScale = devicePixelRatioF() * number(this, "sidebarRenderScale");
    setProperty("settingsCaptureArea", area);
    DesktopCapture::request(area, captureScale, m_registeredWindow, this,
        [this, generation](DesktopCapture::Frame frame) {
            if (generation != m_generation) return;
            m_capturePending = false;
            if (!isVisible() || isMinimized()) return;
            if (!frame.image.isNull()) presentBackdrop(std::move(frame));
        }, m_dragging ? 1 : 0, true, true);
}

void SettingsDialog::presentBackdrop(DesktopCapture::Frame frame)
{
    const QRect area = frame.area;
    const QRect sidebarArea(m_blur->mapToGlobal(QPoint()), m_blur->size());
    // Crop a newly reconstructed desktop frame at the CURRENT position. A pure
    // target translation must not starve presentation while the mouse keeps moving.
    if (!area.contains(sidebarArea) || (!frame.validated && !DesktopCapture::validate(frame)))
        return;
    const qreal scaleX = frame.image.width() / qreal(area.width());
    const qreal scaleY = frame.image.height() / qreal(area.height());
    const QPoint offset = sidebarArea.topLeft() - area.topLeft();
    const QRect crop(qRound(offset.x() * scaleX), qRound(offset.y() * scaleY),
        qRound(sidebarArea.width() * frame.scale), qRound(sidebarArea.height() * frame.scale));
    const QRegion valid = frame.valid.intersected(crop).translated(-crop.topLeft());
    QImage sidebar = crop == frame.image.rect() ? std::move(frame.image) : frame.image.copy(crop);
    if (sidebar.isNull()) return;
    QImage backdrop;
    if (valid == QRegion(sidebar.rect())) {
        backdrop = std::move(sidebar);
    } else {
        // Registered desktop widgets are restored by the compositor below.
        // Unavailable desktop-host pixels use wallpaper, never a cached window frame.
        backdrop = LiquidGlassWidget::desktopWallpaper(sidebarArea, frame.scale);
        if (backdrop.isNull()) {
            backdrop = QImage(sidebar.size(), QImage::Format_RGB32);
            backdrop.fill(color(this, "canvas"));
        }
        QPainter painter(&backdrop);
        painter.setClipRegion(valid);
        painter.drawImage(0, 0, sidebar);
    }
    LiquidGlassWidget::compositeDesktopGlass(backdrop, sidebarArea, m_registeredWindow);
    if (backdrop != m_lastBackdrop) {
        m_lastBackdrop = backdrop;
        static_cast<SettingsBackdrop*>(m_blur)->setBackdrop(backdrop, px(this, "sidebarBlurRadius"));
    }
}

void SettingsDialog::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath outline;
    outline.addRoundedRect(QRectF(rect()),
        px(this, "windowRadius"), px(this, "windowRadius"));
    p.fillPath(outline, color(this, "canvas"));
}

bool SettingsDialog::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_search && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        auto* field = m_search->parentWidget();
        field->setProperty("focused", event->type() == QEvent::FocusIn);
        field->style()->unpolish(field);
        field->style()->polish(field);
        field->update();
    }
    if (object == m_sidebar && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        m_blur->setGeometry(m_sidebar->geometry());
    if (event->type() == QEvent::Resize && object->property("settingsStepper").toBool()) {
        auto* control = qobject_cast<QWidget*>(object);
        if (auto* indicator = control->findChild<QWidget*>(QStringLiteral("settingsStepperIndicator"))) {
            const int d = px(this, "selectorDiameter");
            indicator->setGeometry(control->width() - d, (control->height() - d) / 2, d, d);
        }
    }
    if (event->type() == QEvent::Paint && !object->property("settingsPaint").toString().isEmpty()) {
        if (auto* widget = qobject_cast<QWidget*>(object)) SettingsAppearance::paintControl(this, widget);
        return true;
    }
    if (!object->property("settingsDragRegion").toBool()) return QDialog::eventFilter(object, event);
    auto* region = qobject_cast<QWidget*>(object);
    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && !isMaximized()) {
            m_dragOffset = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
            m_dragging = true;
            m_frameClock->setInteracting(true);
            region->grabMouse();
            return true;
        }
    } else if (event->type() == QEvent::MouseMove && m_dragging) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        move(mouse->globalPosition().toPoint() - m_dragOffset);
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
        m_dragging = false;
        m_frameClock->setInteracting(false);
        region->releaseMouse();
        refreshBackdrop();
        return true;
    } else if (event->type() == QEvent::UngrabMouse) {
        m_dragging = false;
        m_frameClock->setInteracting(false);
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        isMaximized() ? showNormal() : showMaximized();
        return true;
    }
    return QDialog::eventFilter(object, event);
}

bool SettingsDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    auto* native = static_cast<MSG*>(message);
    if (native->message == WM_NCHITTEST && !isMaximized()) {
        RECT bounds{};
        GetWindowRect(native->hwnd, &bounds);
        const int x = short(LOWORD(native->lParam));
        const int y = short(HIWORD(native->lParam));
        const int edge = qRound(px(this, "resizeEdge") * devicePixelRatioF());
        const bool left = x < bounds.left + edge, right = x >= bounds.right - edge;
        const bool top = y < bounds.top + edge, bottom = y >= bounds.bottom - edge;
        const int hit = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
            : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
            : left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
        if (hit != HTCLIENT) { *result = hit; return true; }
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
