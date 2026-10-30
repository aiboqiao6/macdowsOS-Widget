#include "ui/textcontrols.h"
#include "widgets/desktopwidget.h"
#include "ui/scaleselector.h"
#include "ui/settingsdialog.h"
#include "ui/settingsappearance.h"
#include "ui/widgetlibrarydialog.h"
#include "app/appsettings.h"
#include "config.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

void DesktopWidget::showSettingsDialog()
{
    // 桌面卡片属于非激活桌面层，设置使用独立顶层窗口以正常接收焦点。
    SettingsDialog dialog(darkModeEnabled());
    dialog.setInterfaceScale(m_widgetScale);
    auto* generalGroup = dialog.addGroup(SettingsDialog::General, QStringLiteral("桌面交互"));
    auto* appearanceGroup = dialog.addGroup(SettingsDialog::Appearance, QStringLiteral("显示"));
    auto* materialGroup = dialog.addGroup(SettingsDialog::Material, QStringLiteral("材质参数"));
    auto* weatherGroup = dialog.addGroup(SettingsDialog::Weather, QStringLiteral("位置"));
    auto* customGroup = dialog.addGroup(SettingsDialog::Widgets, QStringLiteral("小组件工作室"));
    auto* customEditor = new QPushButton(QStringLiteral("打开工作室…"), &dialog);
    customEditor->setObjectName(QStringLiteral("customWidgetEditor"));
    customEditor->setAutoDefault(false);
    dialog.addRow(customGroup, QStringLiteral("创建你的组件"), customEditor,
        QStringLiteral("使用低代码或 HTML 创建，支持导入和导出。保存后可在组件库中添加到桌面。"));
    connect(customEditor, &QPushButton::clicked, &dialog, [&dialog]() {
        openCustomWidgetEditor({}, &dialog);
    });
    auto* colorMode = new QComboBox(&dialog);
    colorMode->setObjectName(QStringLiteral("colorMode"));
    colorMode->addItem(QStringLiteral("亮色"), false);
    colorMode->addItem(QStringLiteral("暗色"), true);
    colorMode->setCurrentIndex(colorMode->findData(darkModeEnabled()));
    dialog.addRow(appearanceGroup, QStringLiteral("外观模式"), colorMode);
    connect(colorMode, &QComboBox::currentIndexChanged, &dialog, [&] {
        const bool dark = colorMode->currentData().toBool();
        setGlobalDarkMode(dark);
        dialog.setDarkAppearance(dark);
    });
    auto* scaleSelector = new ScaleSelector(qRound(m_widgetScale * 100), &dialog);
    scaleSelector->findChild<QLabel*>(QStringLiteral("scaleValue"))->setStyleSheet(
        QStringLiteral("font-size:20px; font-weight:600; color:#0a84ff;"));
    auto* scaleHint = scaleSelector->findChild<QLabel*>(QStringLiteral("scalePreviewHint"));
    scaleHint->setStyleSheet({});
    scaleHint->setProperty("secondary", true);
    scaleSelector->findChild<QSlider*>(QStringLiteral("scaleSlider"))->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height:5px; border:0; border-radius:2px; background:#86868b; }"
        "QSlider::sub-page:horizontal { border:0; border-radius:2px; background:#0a84ff; }"
        "QSlider::handle:horizontal { width:18px; margin:-7px 0; border:1px solid #b9b9bf; border-radius:9px; background:white; }"));
    auto* scaleGroup = dialog.addGroup(SettingsDialog::Appearance, QStringLiteral("小组件大小"));
    SettingsAppearance::margins(scaleGroup, "groupInset", "rowPadding", "groupInset", "rowPadding");
    scaleGroup->addWidget(scaleSelector);

    auto* renderingGroup = dialog.addGroup(SettingsDialog::Appearance, QStringLiteral("渲染"));
    auto* liveBackdrop = new QCheckBox(&dialog);
    liveBackdrop->setObjectName(QStringLiteral("liveBackdrop"));
    liveBackdrop->setChecked(liveBackdropEnabled());
    dialog.addRow(renderingGroup, QStringLiteral("实时背景"), liveBackdrop,
        QStringLiteral("显示组件下方的窗口内容。关闭时使用桌面壁纸。"));

    auto* qualityBox = new QComboBox(&dialog);
    qualityBox->setObjectName(QStringLiteral("renderQuality"));
    qualityBox->addItem(QStringLiteral("节能 · 60%"), 0.60);
    qualityBox->addItem(QStringLiteral("均衡 · 80%"), 0.80);
    qualityBox->addItem(QStringLiteral("原生 · 100%"), 1.00);
    qualityBox->addItem(QStringLiteral("高质量 · 125%（推荐）"), 1.25);
    qualityBox->addItem(QStringLiteral("极致 · 150%"), 1.50);
    qualityBox->setToolTip(QStringLiteral("按设备像素设置渲染比例，超过 100% 时使用超采样。"));
    int qualityIndex = 2;
    for (int i = 0; i < qualityBox->count(); ++i) {
        if (qAbs(qualityBox->itemData(i).toDouble() - qreal(renderScale())) < 0.001) {
            qualityIndex = i;
            break;
        }
    }
    qualityBox->setCurrentIndex(qualityIndex);
    dialog.addRow(renderingGroup, QStringLiteral("渲染清晰度"), qualityBox);

    auto* imageAntialiasing = new QCheckBox(QStringLiteral("开启（推荐）"), &dialog);
    imageAntialiasing->setObjectName(QStringLiteral("imageAntialiasing"));
    imageAntialiasing->setChecked(imageAntialiasingEnabled());
    dialog.addRow(renderingGroup, QStringLiteral("图像抗锯齿"), imageAntialiasing,
        QStringLiteral("平滑电量圆环、设备图标、时钟指针与图片边缘。调整后立即应用到所有组件。"));

    const MaterialParameters originalMaterial = materialParameters();
    const auto addMaterialNumber = [&](const QString& name, const QString& label,
                                       double minimum, double maximum, int decimals,
                                       double step, double value) {
        auto* number = new AppText::DoubleSpinBox(&dialog);
        number->setObjectName(name);
        number->setRange(minimum, maximum);
        number->setDecimals(decimals);
        number->setSingleStep(step);
        number->setValue(value);
        dialog.addRow(materialGroup, label, number);
        return number;
    };
    auto* materialOpacity = addMaterialNumber(QStringLiteral("materialOpacity"),
        QStringLiteral("材质不透明度"), 5, 100, 0, 1, originalMaterial.opacity * 100.0);
    materialOpacity->setSuffix(QStringLiteral(" %"));
    auto* materialRefraction = addMaterialNumber(QStringLiteral("materialRefraction"),
        QStringLiteral("折射强度"), 0, 3, 2, 0.05, originalMaterial.refraction);
    auto* materialBlur = addMaterialNumber(QStringLiteral("materialBlurRadius"),
        QStringLiteral("模糊半径"), 0, 12, 2, 0.05, originalMaterial.blurRadius);
    auto* materialIterations = new AppText::SpinBox(&dialog);
    materialIterations->setObjectName(QStringLiteral("materialBlurIterations"));
    materialIterations->setRange(1, 6);
    materialIterations->setValue(originalMaterial.blurIterations);
    dialog.addRow(materialGroup, QStringLiteral("模糊次数"), materialIterations);
    auto* materialNoise = addMaterialNumber(QStringLiteral("materialNoise"),
        QStringLiteral("颗粒强度"), 0, 0.1, 3, 0.001, originalMaterial.noise);
    auto* materialInfo = dialog.addGroup(SettingsDialog::Material);
    dialog.addRow(materialInfo, QStringLiteral("实时应用"), nullptr,
        QStringLiteral("调整会立即应用并自动保存。\n较高的模糊设置会增加渲染开销。"));
    auto* materialSaveError = new AppText::Label(
        QStringLiteral("无法保存材质设置，请检查配置目录是否可写后重试。"), &dialog);
    materialSaveError->setObjectName(QStringLiteral("materialSaveError"));
    materialSaveError->setWordWrap(true);
    materialSaveError->hide();
    materialInfo->addWidget(materialSaveError);
    auto* resetMaterial = new QPushButton(QStringLiteral("恢复默认材质"), &dialog);
    resetMaterial->setObjectName(QStringLiteral("resetMaterial"));
    resetMaterial->setAutoDefault(false);
    dialog.addRow(materialInfo, QStringLiteral("还原材质"), resetMaterial);

    const auto selectedMaterial = [=]() {
        return MaterialParameters{float(materialOpacity->value() / 100.0),
            float(materialRefraction->value()), float(materialBlur->value()),
            materialIterations->value(), float(materialNoise->value())};
    };
    const auto applyMaterial = [&]() {
        const MaterialParameters parameters = selectedMaterial();
        AppSettings settings;
        settings.setValue(QStringLiteral("material/opacity"), parameters.opacity);
        settings.setValue(QStringLiteral("material/refraction"), parameters.refraction);
        settings.setValue(QStringLiteral("material/blurRadius"), parameters.blurRadius);
        settings.setValue(QStringLiteral("material/blurIterations"), parameters.blurIterations);
        settings.setValue(QStringLiteral("material/noise"), parameters.noise);
        materialSaveError->setVisible(!settings.sync());
        for (QWidget* widget : QApplication::allWidgets()) {
            auto* surface = qobject_cast<QtGlassFlowScene*>(widget);
            if (surface)
                surface->setMaterialParameters(parameters);
        }
    };
    for (QDoubleSpinBox* number : {materialOpacity, materialRefraction, materialBlur, materialNoise})
        connect(number, &QDoubleSpinBox::valueChanged, &dialog, applyMaterial);
    connect(materialIterations, &QSpinBox::valueChanged, &dialog, applyMaterial);
    connect(resetMaterial, &QPushButton::clicked, &dialog, [&]() {
        const MaterialParameters defaults;
        const QSignalBlocker opacityBlock(materialOpacity), refractionBlock(materialRefraction),
            blurBlock(materialBlur), iterationsBlock(materialIterations), noiseBlock(materialNoise);
        materialOpacity->setValue(defaults.opacity * 100.0);
        materialRefraction->setValue(defaults.refraction);
        materialBlur->setValue(defaults.blurRadius);
        materialIterations->setValue(defaults.blurIterations);
        materialNoise->setValue(defaults.noise);
        applyMaterial();
    });

    auto* locationRow = new QWidget(&dialog);
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(8);
    auto* locationEdit = new AppText::LineEdit(
        AppSettings().value(QStringLiteral("weather/location"), m_weatherLocation).toString(),
        locationRow);
    locationEdit->setReadOnly(true);
    locationEdit->setPlaceholderText(QStringLiteral("请搜索并选择区县"));
    auto* locationButton = new QPushButton(QStringLiteral("搜索区县…"), locationRow);
    locationButton->setAutoDefault(false);
    locationLayout->addWidget(locationEdit, 1);
    locationLayout->addWidget(locationButton);
    dialog.addRow(weatherGroup, QStringLiteral("天气地区"), locationRow);
    connect(locationButton, &QPushButton::clicked, this, [this, locationEdit]() {
        searchWeatherCity();
        locationEdit->setText(
            AppSettings().value(QStringLiteral("weather/location"), m_weatherLocation).toString());
    });

    auto* mouseThrough = new QCheckBox(&dialog);
    mouseThrough->setObjectName(QStringLiteral("mouseThrough"));
    mouseThrough->setChecked(mouseThroughEnabled());
    dialog.addRow(generalGroup, QStringLiteral("鼠标穿透"), mouseThrough,
        QStringLiteral("让点击穿过小组件，直接操作桌面。"));

    auto* lowPower = new QCheckBox(&dialog);
    lowPower->setObjectName(QStringLiteral("lowPowerRefresh"));
    lowPower->setChecked(lowPowerRefreshEnabled());
    lowPower->setEnabled(mouseThrough->isChecked());
    dialog.addRow(generalGroup, QStringLiteral("节能刷新"), lowPower,
        QStringLiteral("鼠标穿透开启时可用，以 4 FPS 降低后台开销。"));

    auto* startupGroup = dialog.addGroup(SettingsDialog::General, QStringLiteral("启动"));
    auto* startup = new QCheckBox(&dialog);
    startup->setObjectName(QStringLiteral("startupEnabled"));
    startup->setChecked(isStartupEnabled());
    dialog.addRow(startupGroup, QStringLiteral("登录时自动启动"), startup,
        QStringLiteral("登录 Windows 后自动显示桌面小组件。"));

    // Each option updates only its affected surfaces. In particular, dragging
    // the scale slider must not rebuild every gallery preview or toggle input.
    QTimer scaleTimer;
    scaleTimer.setSingleShot(true);
    scaleTimer.setInterval(16);
    const auto applyScaleSelection = [&] {
        const qreal scale = scaleSelector->percent() / 100.0;
        bool changed = false;
        for (DesktopWidget* widget : s_instances) {
            if (!qFuzzyCompare(widget->m_widgetScale, scale)) {
                widget->applyScale(scale);
                changed = true;
            }
        }
        if (changed) saveAllConfigurations();
    };
    connect(&scaleTimer, &QTimer::timeout, &dialog, applyScaleSelection);
    connect(scaleSelector->slider(), &QSlider::valueChanged, &dialog, [&] {
        if (scaleSelector->slider()->isSliderDown()) {
            if (!scaleTimer.isActive()) scaleTimer.start();
        } else {
            scaleTimer.stop();
            applyScaleSelection();
            dialog.setInterfaceScale(scaleSelector->percent() / 100.0);
        }
    });
    connect(scaleSelector->slider(), &QSlider::sliderReleased, &dialog, [&] {
        scaleTimer.stop();
        applyScaleSelection();
        dialog.setInterfaceScale(scaleSelector->percent() / 100.0);
    });
    connect(liveBackdrop, &QCheckBox::toggled, &dialog, [=](bool enabled) {
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), enabled);
        for (DesktopWidget* widget : s_instances) {
            widget->setLiveBackdropEnabled(enabled, false);
            if (widget->m_library) widget->m_library->setLiveBackdropEnabled(enabled, false);
        }
    });
    connect(qualityBox, &QComboBox::currentIndexChanged, &dialog, [=] {
        const float quality = qualityBox->currentData().toFloat();
        AppSettings().setValue(QStringLiteral("performance/renderScale"), quality);
        for (DesktopWidget* widget : s_instances) {
            widget->setRenderScale(quality);
            if (widget->m_library) widget->m_library->setRenderScale(quality);
        }
    });
    connect(imageAntialiasing, &QCheckBox::toggled, &dialog, &DesktopWidget::setGlobalImageAntialiasing);
    const auto applyInteraction = [=] {
        const bool through = mouseThrough->isChecked();
        lowPower->setEnabled(through);
        if (!through) {
            const QSignalBlocker blocker(lowPower);
            lowPower->setChecked(false);
        }
        const bool power = through && lowPower->isChecked();
        for (DesktopWidget* widget : s_instances) {
            widget->setMouseThroughEnabled(through);
            widget->setLowPowerRefreshEnabled(power);
        }
    };
    connect(mouseThrough, &QCheckBox::toggled, &dialog, applyInteraction);
    connect(lowPower, &QCheckBox::toggled, &dialog, applyInteraction);
    connect(startup, &QCheckBox::toggled, &dialog, [=](bool enabled) {
        setStartupEnabled(enabled);
        if (m_startupAction) {
            QSignalBlocker blocker(m_startupAction);
            m_startupAction->setChecked(enabled);
        }
    });
    dialog.exec();
    // Closing or pressing Escape also flushes the last coalesced drag value.
    if (scaleTimer.isActive()) {
        scaleTimer.stop();
        applyScaleSelection();
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
    auto* title = new AppText::Label(QStringLiteral("macdowsOS Widget"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600;"));
    layout->addWidget(title);
    auto* text = new AppText::Label(QStringLiteral("雾蓝回针MistBlueSt 版本 %1 版本号 %2")
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
