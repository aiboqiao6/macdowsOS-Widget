#include "widgets/desktopwidget.h"
#include "ui/scaleselector.h"
#include "app/appsettings.h"
#include "config.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

void DesktopWidget::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("macdowsOS Widget 设置"));
    dialog.setModal(true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#171a21; color:#eef2fa; }"
        "QLabel { color:#eef2fa; }"
        "QComboBox { background:#252a35; color:#eef2fa; border:1px solid #596273; border-radius:6px; padding:5px 10px; }"
        "QCheckBox { color:#eef2fa; spacing:8px; }"
        "QDialogButtonBox QPushButton { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 18px; }"
        "QDialogButtonBox QPushButton:hover { background:#424c60; }"));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("显示、材质与启动"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    layout->addWidget(title);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QWidget#settingsBody { background:#171a21; border:0; }"));
    auto* settingsBody = new QWidget(scroll);
    settingsBody->setObjectName(QStringLiteral("settingsBody"));
    auto* form = new QFormLayout(settingsBody);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    auto* scaleSelector = new ScaleSelector(qRound(m_widgetScale * 100), &dialog);
    form->addRow(QStringLiteral("显示大小"), scaleSelector);

    auto* liveBackdrop = new QCheckBox(QStringLiteral("实时渲染窗口后的内容"), &dialog);
    liveBackdrop->setChecked(liveBackdropEnabled());
    form->addRow(QStringLiteral("背景采样"), liveBackdrop);

    auto* qualityBox = new QComboBox(&dialog);
    qualityBox->addItem(QStringLiteral("节能（60% 设备像素）"), 0.60);
    qualityBox->addItem(QStringLiteral("均衡（80% 设备像素）"), 0.80);
    qualityBox->addItem(QStringLiteral("原生（100% 设备像素）"), 1.00);
    qualityBox->addItem(QStringLiteral("高质量（125% 超采样，推荐）"), 1.25);
    qualityBox->addItem(QStringLiteral("极致（150% 超采样）"), 1.50);
    int qualityIndex = 2;
    for (int i = 0; i < qualityBox->count(); ++i) {
        if (qAbs(qualityBox->itemData(i).toDouble() - qreal(renderScale())) < 0.001) {
            qualityIndex = i;
            break;
        }
    }
    qualityBox->setCurrentIndex(qualityIndex);
    form->addRow(QStringLiteral("渲染清晰度"), qualityBox);

    auto* fontSmoothingBox = new QComboBox(&dialog);
    fontSmoothingBox->addItem(QStringLiteral("关闭"), 0);
    fontSmoothingBox->addItem(QStringLiteral("标准（灰阶）"), 1);
    fontSmoothingBox->addItem(QStringLiteral("清晰（灰阶）"), 2);
    fontSmoothingBox->addItem(QStringLiteral("最高（原生像素灰阶）"), 3);
    fontSmoothingBox->addItem(QStringLiteral("ClearType（子像素，推荐）"), 4);
    fontSmoothingBox->setCurrentIndex(
        qMax(0, fontSmoothingBox->findData(globalFontSmoothing())));
    form->addRow(QStringLiteral("字体抗锯齿"), fontSmoothingBox);

    auto* locationRow = new QWidget(&dialog);
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(8);
    auto* locationEdit = new QLineEdit(
        AppSettings().value(QStringLiteral("weather/location"), m_weatherLocation).toString(),
        locationRow);
    locationEdit->setReadOnly(true);
    locationEdit->setPlaceholderText(QStringLiteral("请搜索并选择区县"));
    auto* locationButton = new QPushButton(QStringLiteral("搜索区县…"), locationRow);
    locationLayout->addWidget(locationEdit, 1);
    locationLayout->addWidget(locationButton);
    form->addRow(QStringLiteral("天气地区"), locationRow);
    connect(locationButton, &QPushButton::clicked, this, [this, locationEdit]() {
        searchWeatherCity();
        locationEdit->setText(
            AppSettings().value(QStringLiteral("weather/location"), m_weatherLocation).toString());
    });

    auto* blurRow = new QWidget(&dialog);
    auto* blurLayout = new QHBoxLayout(blurRow);
    blurLayout->setContentsMargins(0, 0, 0, 0);
    blurLayout->setSpacing(8);
    auto* blurSlider = new QSlider(Qt::Horizontal, blurRow);
    blurSlider->setRange(0, 100);
    blurSlider->setValue(materialBlurStrength());
    auto* blurSpin = new QSpinBox(blurRow);
    blurSpin->setRange(0, 100);
    blurSpin->setSuffix(QStringLiteral("%"));
    blurSpin->setValue(blurSlider->value());
    blurLayout->addWidget(blurSlider, 1);
    blurLayout->addWidget(blurSpin);
    connect(blurSlider, &QSlider::valueChanged,
            blurSpin, &QSpinBox::setValue);
    connect(blurSpin, qOverload<int>(&QSpinBox::valueChanged),
            blurSlider, &QSlider::setValue);
    form->addRow(QStringLiteral("模糊程度"), blurRow);

    auto* opacityRow = new QWidget(&dialog);
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->setSpacing(8);
    auto* opacitySlider = new QSlider(Qt::Horizontal, opacityRow);
    opacitySlider->setRange(5, 100);
    opacitySlider->setValue(qBound(5, qRound(glassOpacity() * 100.0), 100));
    auto* opacitySpin = new QSpinBox(opacityRow);
    opacitySpin->setRange(5, 100);
    opacitySpin->setSuffix(QStringLiteral("%"));
    opacitySpin->setValue(opacitySlider->value());
    opacityLayout->addWidget(opacitySlider, 1);
    opacityLayout->addWidget(opacitySpin);
    connect(opacitySlider, &QSlider::valueChanged,
            opacitySpin, &QSpinBox::setValue);
    connect(opacitySpin, qOverload<int>(&QSpinBox::valueChanged),
            opacitySlider, &QSlider::setValue);
    form->addRow(QStringLiteral("透明度"), opacityRow);

    auto* mouseThrough = new QCheckBox(QStringLiteral("鼠标穿透（组件不接收点击）"), &dialog);
    mouseThrough->setChecked(mouseThroughEnabled());
    form->addRow(QString(), mouseThrough);

    auto* lowPower = new QCheckBox(QStringLiteral("节能刷新（仅鼠标穿透时生效，4 FPS）"), &dialog);
    lowPower->setChecked(lowPowerRefreshEnabled());
    lowPower->setEnabled(mouseThrough->isChecked());
    connect(mouseThrough, &QCheckBox::toggled,
            lowPower, &QWidget::setEnabled);
    form->addRow(QString(), lowPower);

    auto* startup = new QCheckBox(QStringLiteral("登录 Windows 时自动启动"), &dialog);
    startup->setChecked(isStartupEnabled());
    form->addRow(QString(), startup);
    scroll->setWidget(settingsBody);
    layout->addWidget(scroll, 1);

    auto* hint = new QLabel(QStringLiteral("关闭实时背景时使用壁纸采样；开启后显示组件下方的窗口。字体始终按屏幕像素绘制；ClearType 适合 RGB 排列屏幕，旋转屏幕或 BGR 屏幕可选择灰阶模式。"), &dialog);
    hint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // 高 DPI 下逻辑工作区较小，滚动设置项，始终保留底部确认与取消按钮。
    const QSize available = dialog.screen()->availableGeometry().size() - QSize(32, 32);
    dialog.setMaximumSize(available);
    dialog.resize(QSize(640, 740).boundedTo(available));

    if (dialog.exec() == QDialog::Accepted) {
        setStartupEnabled(startup->isChecked());
        syncGroupSettings(scaleSelector->percent() / 100.0,
                          blurSlider->value(),
                          opacitySlider->value() / 100.0,
                          mouseThrough->isChecked(),
                          lowPower->isChecked() && mouseThrough->isChecked(),
                          liveBackdrop->isChecked(),
                          qualityBox->currentData().toDouble(),
                          fontSmoothingBox->currentData().toInt());
        if (m_startupAction) {
            QSignalBlocker blocker(m_startupAction);
            m_startupAction->setChecked(startup->isChecked());
        }
    }
}

bool DesktopWidget::isStartupEnabled() const
{
#ifdef Q_OS_WIN
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QSettings::NativeFormat);
    return runKey.contains(QStringLiteral("macdowsOS Widget"))
           || runKey.contains(QStringLiteral("macdowsOSBattery"));
#else
    return false;
#endif
}

void DesktopWidget::setStartupEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QSettings::NativeFormat);
    constexpr auto kStartupName = "macdowsOS Widget";
    if (enabled) {
        const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        runKey.setValue(QString::fromLatin1(kStartupName), QStringLiteral("\"%1\"").arg(path));
    } else {
        runKey.remove(QString::fromLatin1(kStartupName));
        runKey.remove(QStringLiteral("macdowsOSBattery"));
    }
    runKey.sync();
    AppSettings().setValue(QStringLiteral("startup/enabled"), enabled);
#else
    Q_UNUSED(enabled);
#endif
}

void DesktopWidget::showAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("关于 macdowsOS Widget"));
    dialog.setModal(true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#171a21; color:#eef2fa; }"
        "QLabel { color:#eef2fa; }"
        "QDialogButtonBox QPushButton { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 18px; }"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 24, 28, 20);
    auto* title = new QLabel(QStringLiteral("macdowsOS Widget"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600;"));
    layout->addWidget(title);
    auto* text = new QLabel(QStringLiteral("雾蓝回针MistBlueSt 版本 %1 版本号 %2")
        .arg(QString::fromLatin1(AppConfig::Version),
            QString::fromLatin1(AppConfig::BuildId)), &dialog);
    text->setStyleSheet(QStringLiteral("color:#aab5c8; line-height:1.4;"));
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
