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
#include <QDoubleSpinBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

void DesktopWidget::showSettingsDialog()
{
    // 桌面卡片属于非激活桌面层，设置使用独立顶层窗口以正常接收焦点。
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("macdowsOS Widget 设置"));
    dialog.setModal(true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#171a21; color:#eef2fa; }"
        "QLabel { color:#eef2fa; }"
        "QComboBox { background:#252a35; color:#eef2fa; border:1px solid #596273; border-radius:6px; padding:5px 10px; }"
        "QCheckBox { color:#eef2fa; spacing:8px; }"
        "QDoubleSpinBox, QSpinBox { background:#252a35; color:#eef2fa; border:1px solid #596273; border-radius:6px; padding:5px 8px; }"
        "QPushButton#resetMaterial, QPushButton#customWidgetEditor { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 12px; }"
        "QPushButton#customWidgetEditor:hover { background:#424c60; }"
        "QDialogButtonBox QPushButton { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 18px; }"
        "QDialogButtonBox QPushButton:hover { background:#424c60; }"));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("显示与启动"), &dialog);
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
    auto* customEditor = new QPushButton(QStringLiteral("打开小组件工作室…"), settingsBody);
    customEditor->setObjectName(QStringLiteral("customWidgetEditor"));
    form->addRow(QStringLiteral("自定义小组件"), customEditor);
    auto* customHint = new QLabel(QStringLiteral("低代码或 HTML 创建组件，支持导入、导出。保存后会出现在添加窗口的“自定义小组件”分区。"), settingsBody);
    customHint->setWordWrap(true);
    customHint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    form->addRow(customHint);
    connect(customEditor, &QPushButton::clicked, &dialog, [&dialog]() {
        openCustomWidgetEditor({}, &dialog);
    });
    auto* colorMode = new QComboBox(&dialog);
    colorMode->setObjectName(QStringLiteral("colorMode"));
    colorMode->addItem(QStringLiteral("亮色"), false);
    colorMode->addItem(QStringLiteral("暗色"), true);
    colorMode->setCurrentIndex(colorMode->findData(darkModeEnabled()));
    form->addRow(QStringLiteral("外观模式"), colorMode);
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

    const MaterialParameters originalMaterial = materialParameters();
    auto* materialTitle = new QLabel(QStringLiteral("小组件材质"), &dialog);
    materialTitle->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600; margin-top:12px;"));
    form->addRow(materialTitle);
    const auto addMaterialNumber = [&](const QString& name, const QString& label,
                                       double minimum, double maximum, int decimals,
                                       double step, double value) {
        auto* number = new QDoubleSpinBox(&dialog);
        number->setObjectName(name);
        number->setRange(minimum, maximum);
        number->setDecimals(decimals);
        number->setSingleStep(step);
        number->setKeyboardTracking(false);
        number->setValue(value);
        form->addRow(label, number);
        return number;
    };
    auto* materialOpacity = addMaterialNumber(QStringLiteral("materialOpacity"),
        QStringLiteral("材质不透明度"), 5, 100, 0, 1, originalMaterial.opacity * 100.0);
    materialOpacity->setSuffix(QStringLiteral(" %"));
    auto* materialRefraction = addMaterialNumber(QStringLiteral("materialRefraction"),
        QStringLiteral("折射强度"), 0, 3, 2, 0.05, originalMaterial.refraction);
    auto* materialBlur = addMaterialNumber(QStringLiteral("materialBlurRadius"),
        QStringLiteral("模糊半径"), 0, 12, 2, 0.05, originalMaterial.blurRadius);
    auto* materialIterations = new QSpinBox(&dialog);
    materialIterations->setObjectName(QStringLiteral("materialBlurIterations"));
    materialIterations->setRange(1, 6);
    materialIterations->setValue(originalMaterial.blurIterations);
    materialIterations->setKeyboardTracking(false);
    form->addRow(QStringLiteral("模糊次数"), materialIterations);
    auto* materialNoise = addMaterialNumber(QStringLiteral("materialNoise"),
        QStringLiteral("颗粒强度"), 0, 0.1, 3, 0.001, originalMaterial.noise);
    auto* materialHint = new QLabel(QStringLiteral("材质调整即时预览并统一应用到玻璃表面，不影响文字不透明度。模糊半径和次数越大，渲染开销越高。"), &dialog);
    materialHint->setWordWrap(true);
    materialHint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    form->addRow(materialHint);
    auto* resetMaterial = new QPushButton(QStringLiteral("恢复默认材质"), &dialog);
    resetMaterial->setObjectName(QStringLiteral("resetMaterial"));
    form->addRow(QString(), resetMaterial);

    const auto selectedMaterial = [=]() {
        return MaterialParameters{float(materialOpacity->value() / 100.0),
            float(materialRefraction->value()), float(materialBlur->value()),
            materialIterations->value(), float(materialNoise->value())};
    };
    // 每个表面单独记录预览前的值；取消时也恢复设置期间新出现的表面。
    QList<QPair<QPointer<QtGlassFlowScene>, MaterialParameters>> materialSnapshots;
    const auto previewMaterial = [&]() {
        const MaterialParameters parameters = selectedMaterial();
        for (QWidget* widget : QApplication::allWidgets()) {
            auto* surface = qobject_cast<QtGlassFlowScene*>(widget);
            if (!surface)
                continue;
            bool recorded = false;
            for (const auto& snapshot : materialSnapshots)
                recorded = recorded || snapshot.first == surface;
            if (!recorded)
                materialSnapshots.append({surface, surface->materialParameters()});
            surface->setMaterialParameters(parameters);
        }
    };
    for (QDoubleSpinBox* number : {materialOpacity, materialRefraction, materialBlur, materialNoise})
        connect(number, &QDoubleSpinBox::valueChanged, &dialog, previewMaterial);
    connect(materialIterations, &QSpinBox::valueChanged, &dialog, previewMaterial);
    connect(resetMaterial, &QPushButton::clicked, &dialog, [&]() {
        const MaterialParameters defaults;
        const QSignalBlocker opacityBlock(materialOpacity), refractionBlock(materialRefraction),
            blurBlock(materialBlur), iterationsBlock(materialIterations), noiseBlock(materialNoise);
        materialOpacity->setValue(defaults.opacity * 100.0);
        materialRefraction->setValue(defaults.refraction);
        materialBlur->setValue(defaults.blurRadius);
        materialIterations->setValue(defaults.blurIterations);
        materialNoise->setValue(defaults.noise);
        previewMaterial();
    });
    const auto restoreMaterial = [&]() {
        for (const auto& snapshot : materialSnapshots)
            if (snapshot.first)
                snapshot.first->setMaterialParameters(snapshot.second);
    };

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
        const MaterialParameters material = selectedMaterial();
        AppSettings settings;
        settings.setValue(QStringLiteral("material/opacity"), material.opacity);
        settings.setValue(QStringLiteral("material/refraction"), material.refraction);
        settings.setValue(QStringLiteral("material/blurRadius"), material.blurRadius);
        settings.setValue(QStringLiteral("material/blurIterations"), material.blurIterations);
        settings.setValue(QStringLiteral("material/noise"), material.noise);
        if (!settings.sync()) {
            restoreMaterial();
            QMessageBox::warning(this, QStringLiteral("保存失败"),
                QStringLiteral("无法保存材质设置，请检查配置目录是否可写后重试。"));
            return;
        }
        previewMaterial();
        setGlobalDarkMode(colorMode->currentData().toBool());
        setStartupEnabled(startup->isChecked());
        syncGroupSettings(scaleSelector->percent() / 100.0,
                          mouseThrough->isChecked(),
                          lowPower->isChecked() && mouseThrough->isChecked(),
                          liveBackdrop->isChecked(),
                          qualityBox->currentData().toDouble(),
                          fontSmoothingBox->currentData().toInt());
        if (m_startupAction) {
            QSignalBlocker blocker(m_startupAction);
            m_startupAction->setChecked(startup->isChecked());
        }
    } else {
        restoreMaterial();
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
