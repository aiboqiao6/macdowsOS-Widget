#include "app/fontsettings.h"
#include "ui/textcontrols.h"
#include "ui/widgeteditordialog.h"
#include "ui/widgetlibrarydialog.h"
#include "ui/htmlwidgetview.h"
#include "widgets/desktopwidget.h"
#include "app/appsettings.h"
#include <QAction>
#include <QAbstractItemModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

namespace {
QString blockName(const QJsonObject& block) {
    const QString type = block.value("type").toString();
    const QString name = type == "clock" ? QStringLiteral("时钟") : type == "date" ? QStringLiteral("日期")
        : type == "progress" ? QStringLiteral("进度条") : type == "divider" ? QStringLiteral("分隔线")
        : type == "metric" ? QStringLiteral("大数字") : QStringLiteral("文本");
    return name + (block.value("text").toString().isEmpty() ? QString() : " · " + block.value("text").toString().simplified().left(22));
}
QPushButton* button(const QString& text, QBoxLayout* layout) {
    auto* result = new QPushButton(text);
    result->setAutoDefault(false);
    result->setDefault(false);
    result->setCursor(Qt::PointingHandCursor);
    layout->addWidget(result);
    return result;
}
QByteArray snapshot(const CustomWidgets::Definition& definition) {
    return QJsonDocument(definition.toJson()).toJson(QJsonDocument::Compact);
}
}

WidgetEditorDialog::WidgetEditorDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("widgetEditor");
    setWindowTitle(QStringLiteral("小组件工作室"));
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(720, 540);
    const QRect area = QApplication::primaryScreen()->availableGeometry();
    resize(qMin(1080, area.width() - 48), qMin(780, area.height() - 48));
    setStyleSheet(QStringLiteral(
        "QDialog#widgetEditor{background:#101827;color:#eaf0fb}"
        "QLabel,QCheckBox{color:#dce6f8}"
        "QLineEdit,QPlainTextEdit,QListWidget,QComboBox,QSpinBox{background:#1b273c;color:#eaf0fb;border:1px solid #374861;border-radius:6px;padding:5px;selection-background-color:#3b64a4}"
        "QListWidget::item{padding:6px}QListWidget::item:selected{background:#345b99;border-radius:5px}"
        "QPushButton,QToolButton{background:#253753;color:#eff5ff;border:1px solid #3a5276;border-radius:7px;padding:7px 10px}"
        "QPushButton:hover,QToolButton:hover{background:#34517b}"
        "QPushButton#addCustomWidgetDesktop{background:#78aaff;color:#10203a;border:0;font-weight:bold}"
        "QPushButton:disabled{color:#6d809c}QScrollArea{border:0;background:transparent}"
        "QTabWidget::pane{border:1px solid #31415b}QTabBar::tab{background:#1b273c;color:#dce6f8;padding:7px 12px}"
        "QTabBar::tab:selected{background:#34517b}QSplitter::handle{background:#101827;width:8px}"
        "QMenu{background:#1b273c;color:#eaf0fb;border:1px solid #374861;padding:5px}"
        "QMenu::item{padding:7px 20px}QMenu::item:selected{background:#34517b}"
        "QMenu::item:disabled{color:#6d809c}"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* library = new QHBoxLayout;
    library->addWidget(new AppText::Label(QStringLiteral("我的组件")));
    m_catalog = new QComboBox;
    m_catalog->setObjectName("customCatalog");
    m_catalog->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_catalog->setMinimumContentsLength(12);
    library->addWidget(m_catalog, 1);
    auto* create = new QToolButton;
    create->setText(QStringLiteral("新建"));
    create->setPopupMode(QToolButton::InstantPopup);
    create->setCursor(Qt::PointingHandCursor);
    auto* templates = new QMenu(create);
    const QStringList templateNames{QStringLiteral("便签"), QStringLiteral("时钟与日期"), QStringLiteral("目标进度"), QStringLiteral("HTML 计数器")};
    for (int i = 0; i < templateNames.size(); ++i) {
        auto* action = templates->addAction(templateNames[i]);
        connect(action, &QAction::triggered, this, [this, i]() { newDraft(i); });
    }
    create->setMenu(templates);
    library->addWidget(create);
    auto* importButton = button(QStringLiteral("导入"), library);
    importButton->setToolTip(QStringLiteral("导入 JSON 或 HTML 文件，立即加入组件库"));
    auto* more = new QToolButton;
    more->setText(QStringLiteral("更多"));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setCursor(Qt::PointingHandCursor);
    auto* moreMenu = new QMenu(more);
    auto* exportAction = moreMenu->addAction(QStringLiteral("导出 JSON…"));
    auto* cloneAction = moreMenu->addAction(QStringLiteral("创建副本"));
    moreMenu->addSeparator();
    m_deleteAction = moreMenu->addAction(QStringLiteral("从组件库删除…"));
    more->setMenu(moreMenu);
    library->addWidget(more);
    root->addLayout(library);

    m_splitter = new QSplitter;
    m_splitter->setChildrenCollapsible(false);
    root->addWidget(m_splitter, 1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(350);
    auto* edit = new QWidget;
    edit->setStyleSheet("background:#101827");
    auto* editLayout = new QVBoxLayout(edit);
    editLayout->setContentsMargins(0, 0, 8, 0);
    editLayout->setSpacing(10);

    auto* basics = new QHBoxLayout;
    basics->setSpacing(8);
    auto basicField = [basics](const QString& label, QWidget* field, int stretch) {
        auto* column = new QVBoxLayout;
        column->setSpacing(4);
        column->addWidget(new AppText::Label(label));
        column->addWidget(field);
        basics->addLayout(column, stretch);
    };
    m_name = new AppText::LineEdit;
    m_name->setMaxLength(80);
    m_name->setObjectName("customName");
    m_name->setPlaceholderText(QStringLiteral("组件名称"));
    m_mode = new QComboBox;
    m_mode->addItems({QStringLiteral("低代码"), QStringLiteral("HTML")});
    m_mode->setObjectName("customMode");
    m_size = new QComboBox;
    m_size->addItems({QStringLiteral("小 · 1×1"), QStringLiteral("中 · 2×1"), QStringLiteral("大 · 2×2")});
    m_size->setObjectName("customSize");
    basicField(QStringLiteral("名称"), m_name, 1);
    basicField(QStringLiteral("编辑方式"), m_mode, 0);
    basicField(QStringLiteral("尺寸"), m_size, 0);
    editLayout->addLayout(basics);

    m_pages = new QStackedWidget;
    auto* visual = new QWidget;
    auto* visualLayout = new QVBoxLayout(visual);
    visualLayout->setContentsMargins(0, 0, 0, 0);
    visualLayout->setSpacing(8);
    visualLayout->addWidget(new AppText::Label(QStringLiteral("内容块")));
    m_blocks = new QListWidget;
    m_blocks->setObjectName("customBlocks");
    m_blocks->setMinimumHeight(110);
    m_blocks->setMaximumHeight(160);
    m_blocks->setDragDropMode(QAbstractItemView::InternalMove);
    m_blocks->setDefaultDropAction(Qt::MoveAction);
    m_blocks->setToolTip(QStringLiteral("选择内容块编辑，拖动可调整顺序"));
    visualLayout->addWidget(m_blocks);
    auto* addRow = new QHBoxLayout;
    m_blockType = new QComboBox;
    const QStringList types{"text", "metric", "clock", "date", "progress", "divider"};
    const QStringList names{QStringLiteral("文本"), QStringLiteral("大数字"), QStringLiteral("时钟"), QStringLiteral("日期"), QStringLiteral("进度条"), QStringLiteral("分隔线")};
    for (int i = 0; i < types.size(); ++i) m_blockType->addItem(names[i], types[i]);
    addRow->addWidget(m_blockType, 1);
    auto* add = button(QStringLiteral("添加"), addRow);
    add->setObjectName("addCustomBlock");
    visualLayout->addLayout(addRow);
    auto* order = new QHBoxLayout;
    m_upButton = button(QStringLiteral("上移"), order);
    m_downButton = button(QStringLiteral("下移"), order);
    m_duplicateBlockButton = button(QStringLiteral("复制"), order);
    m_deleteBlockButton = button(QStringLiteral("删除"), order);
    visualLayout->addLayout(order);
    m_blockHint = new AppText::Label;
    m_blockHint->setWordWrap(true);
    m_blockHint->setStyleSheet("color:#91a5c4");
    visualLayout->addWidget(m_blockHint);
    m_blockForm = new QFormLayout;
    m_blockForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_blockText = new QPlainTextEdit;
    m_blockText->setMaximumHeight(80);
    m_blockText->setPlaceholderText(QStringLiteral("输入文字或数字，支持换行"));
    m_blockText->setObjectName("customBlockText");
    m_fontSize = new AppText::SpinBox;
    m_fontSize->setRange(10, 96);
    m_fontSize->setSuffix(" px");
    m_align = new QComboBox;
    m_align->addItems({QStringLiteral("左对齐"), QStringLiteral("居中"), QStringLiteral("右对齐")});
    m_bold = new QCheckBox(QStringLiteral("加粗"));
    m_progress = new AppText::SpinBox;
    m_progress->setRange(0, 100);
    m_progress->setSuffix(" %");
    m_blockForm->addRow(QStringLiteral("内容"), m_blockText);
    m_blockForm->addRow(QStringLiteral("字号"), m_fontSize);
    m_blockForm->addRow(QStringLiteral("对齐"), m_align);
    m_blockForm->addRow(QStringLiteral("字重"), m_bold);
    m_blockForm->addRow(QStringLiteral("完成进度"), m_progress);
    visualLayout->addLayout(m_blockForm);

    auto* styleToggle = new QToolButton;
    styleToggle->setText(QStringLiteral("颜色与边距"));
    styleToggle->setCheckable(true);
    styleToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    styleToggle->setArrowType(Qt::RightArrow);
    styleToggle->setCursor(Qt::PointingHandCursor);
    visualLayout->addWidget(styleToggle, 0, Qt::AlignLeft);
    m_stylePanel = new QWidget;
    auto* style = new QFormLayout(m_stylePanel);
    style->setContentsMargins(0, 2, 0, 0);
    auto addColor = [this, style](const QString& label) {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* field = new AppText::LineEdit;
        field->setMaximumWidth(160);
        layout->addWidget(field, 1);
        auto* choose = button(QStringLiteral("选色"), layout);
        connect(choose, &QPushButton::clicked, this, [this, field]() {
            const auto selected = QColorDialog::getColor(QColor(field->text()), this, QStringLiteral("选择颜色"));
            if (selected.isValid()) field->setText(selected.name());
        });
        connect(field, &QLineEdit::textChanged, this, [this]() { changed(); });
        style->addRow(label, row);
        return field;
    };
    m_background = addColor(QStringLiteral("背景颜色"));
    m_foreground = addColor(QStringLiteral("文字颜色"));
    m_accent = addColor(QStringLiteral("强调颜色"));
    m_padding = new AppText::SpinBox;
    m_padding->setRange(0, 64);
    m_padding->setSuffix(" px");
    style->addRow(QStringLiteral("内容边距"), m_padding);
    visualLayout->addWidget(m_stylePanel);
    m_stylePanel->hide();
    connect(styleToggle, &QToolButton::toggled, this, [this, styleToggle](bool visible) {
        m_stylePanel->setVisible(visible);
        styleToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    });
    visualLayout->addStretch();
    m_pages->addWidget(visual);

    auto* code = new QWidget;
    auto* codeLayout = new QVBoxLayout(code);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(8);
    auto* help = new AppText::Label(QStringLiteral("粘贴 HTML 内容，可在 CSS 和 JavaScript 标签中补充样式与交互。"));
    help->setWordWrap(true);
    help->setStyleSheet("color:#91a5c4");
    codeLayout->addWidget(help);
    auto* tabs = new QTabWidget;
    codeLayout->addWidget(tabs, 1);
    auto editor = [tabs](const QString& label) {
        auto* field = new QPlainTextEdit;
        QFont font(AppFonts::family());
        font.setPointSize(10);
        field->setFont(font);
        field->setLineWrapMode(QPlainTextEdit::NoWrap);
        field->setTabStopDistance(28);
        tabs->addTab(field, label);
        return field;
    };
    m_html = editor("HTML");
    m_html->setObjectName("customHtml");
    m_css = editor("CSS");
    m_script = editor("JavaScript");
    tabs->setMinimumHeight(300);
    m_network = new QCheckBox(QStringLiteral("允许加载在线资源"));
    m_network->setToolTip(QStringLiteral("加载在线图片、字体或请求 HTTP/HTTPS 数据时启用。相对路径请改为绝对 URL 或 data URL。"));
    codeLayout->addWidget(m_network);
    m_pages->addWidget(code);
    editLayout->addWidget(m_pages, 1);
    scroll->setWidget(edit);
    m_splitter->addWidget(scroll);

    auto* previewPanel = new QWidget;
    previewPanel->setMinimumWidth(250);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(6, 0, 0, 0);
    previewLayout->setSpacing(8);
    auto* previewHeader = new QHBoxLayout;
    previewHeader->addWidget(new AppText::Label(QStringLiteral("实时预览")));
    previewHeader->addStretch();
    auto* refresh = button(QStringLiteral("重载"), previewHeader);
    refresh->setToolTip(QStringLiteral("重新运行 HTML 和脚本，或重试启动预览"));
    previewLayout->addLayout(previewHeader);
    m_previewSize = new AppText::Label;
    m_previewSize->setWordWrap(true);
    m_previewSize->setStyleSheet("color:#91a5c4");
    previewLayout->addWidget(m_previewSize);
    m_previewArea = new QScrollArea;
    m_previewArea->setWidgetResizable(true);
    m_previewArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_previewArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_previewArea->viewport()->installEventFilter(this);
    auto* previewHost = new QWidget;
    previewHost->setStyleSheet("background:#202e45;border-radius:12px");
    auto* hostLayout = new QVBoxLayout(previewHost);
    hostLayout->setContentsMargins(12, 12, 12, 12);
    hostLayout->setAlignment(Qt::AlignCenter);
    m_preview = new HtmlWidgetView;
    m_preview->setObjectName("customPreview");
    hostLayout->addWidget(m_preview, 0, Qt::AlignCenter);
    m_previewArea->setWidget(previewHost);
    previewLayout->addWidget(m_previewArea, 1);
    m_splitter->addWidget(previewPanel);
    m_splitter->setSizes({570, 450});
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);

    m_status = new AppText::Label;
    m_status->setWordWrap(true);
    m_status->setObjectName("customStatus");
    m_status->setStyleSheet("color:#a8b9d3");
    root->addWidget(m_status);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(6);
    m_undoButton = button(QStringLiteral("撤销"), footer);
    m_undoButton->setToolTip(QStringLiteral("Ctrl+Z"));
    m_redoButton = button(QStringLiteral("重做"), footer);
    m_redoButton->setToolTip(QStringLiteral("Ctrl+Shift+Z / Ctrl+Y"));
    footer->addStretch();
    auto* done = button(QStringLiteral("关闭"), footer);
    m_saveButton = button(QStringLiteral("保存"), footer);
    m_saveButton->setObjectName("saveCustomWidget");
    m_saveButton->setToolTip(QStringLiteral("Ctrl+S · 保存到组件库并同步桌面实例"));
    auto* addDesktop = button(QStringLiteral("保存并添加到桌面"), footer);
    addDesktop->setObjectName("addCustomWidgetDesktop");
    root->addLayout(footer);

    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(300);
    connect(&m_previewTimer, &QTimer::timeout, this, &WidgetEditorDialog::updatePreview);
    m_preview->statusChanged = [this](const QString& status) {
        previewStatusChanged(status);
    };
    connect(importButton, &QPushButton::clicked, this, &WidgetEditorDialog::importDraft);
    connect(exportAction, &QAction::triggered, this, &WidgetEditorDialog::exportDraft);
    connect(cloneAction, &QAction::triggered, this, &WidgetEditorDialog::duplicateDraft);
    connect(m_deleteAction, &QAction::triggered, this, &WidgetEditorDialog::deleteDefinition);
    connect(m_catalog, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_catalogLoading || index < 0) return;
        if (index == 0) {
            if (!confirmDiscard()) { refreshCatalog(m_draft.id); return; }
            loadDraft(CustomWidgets::makeTemplate());
            m_baseline.clear();
            changed();
            return;
        }
        if (index > m_definitions.size()) return;
        const auto selected = m_definitions[index - 1];
        if (selected.id == m_draft.id) return;
        if (!confirmDiscard()) { refreshCatalog(m_draft.id); return; }
        loadDraft(selected);
        refreshCatalog(selected.id);
    });
    connect(m_name, &QLineEdit::textChanged, this, [this]() { changed(); });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_pages->setCurrentIndex(index);
        changed();
    });
    connect(m_size, &QComboBox::currentIndexChanged, this, [this]() { changed(); });
    connect(m_padding, &QSpinBox::valueChanged, this, [this]() { changed(); });
    connect(m_network, &QCheckBox::toggled, this, [this]() { changed(); });
    for (auto* field : {m_html, m_css, m_script})
        connect(field, &QPlainTextEdit::textChanged, this, [this]() { changed(); });
    connect(m_blocks, &QListWidget::currentRowChanged, this, [this]() { loadBlock(); });
    connect(m_blocks->model(), &QAbstractItemModel::rowsMoved, this,
        [this](const QModelIndex&, int first, int, const QModelIndex&, int destination) {
            if (m_loading) return;
            const int target = destination > first ? destination - 1 : destination;
            m_draft.blocks.move(first, target);
            loadBlock(); changed();
        });
    connect(add, &QPushButton::clicked, this, [this]() {
        if (m_draft.blocks.size() >= 40) { m_status->setText(QStringLiteral("最多可添加 40 个内容块。")); return; }
        const QString type = m_blockType->currentData().toString();
        const QString text = type == "text" || type == "metric" || type == "progress" ? QStringLiteral("新内容") : QString();
        QJsonObject block{{"type", type}, {"text", text}, {"size", 18}, {"value", 50}, {"align", "left"}};
        m_draft.blocks.append(block);
        m_blocks->addItem(blockName(block));
        m_blocks->setCurrentRow(m_blocks->count() - 1);
        changed();
    });
    auto move = [this](int offset) {
        const int row = m_blocks->currentRow(), target = row + offset;
        if (row < 0 || target < 0 || target >= m_draft.blocks.size()) return;
        m_draft.blocks.swapItemsAt(row, target);
        m_loading = true;
        auto* item = m_blocks->takeItem(row);
        m_blocks->insertItem(target, item);
        m_loading = false;
        m_blocks->setCurrentRow(target);
        loadBlock();
        changed();
    };
    connect(m_upButton, &QPushButton::clicked, this, [move]() { move(-1); });
    connect(m_downButton, &QPushButton::clicked, this, [move]() { move(1); });
    connect(m_duplicateBlockButton, &QPushButton::clicked, this, [this]() {
        const int row = m_blocks->currentRow();
        if (row < 0 || m_draft.blocks.size() >= 40) return;
        const auto block = m_draft.blocks[row];
        m_draft.blocks.insert(row + 1, block);
        m_blocks->insertItem(row + 1, blockName(block));
        m_blocks->setCurrentRow(row + 1);
        changed();
    });
    connect(m_deleteBlockButton, &QPushButton::clicked, this, [this]() {
        const int row = m_blocks->currentRow();
        if (row < 0) return;
        m_loading = true;
        m_draft.blocks.removeAt(row);
        delete m_blocks->takeItem(row);
        m_loading = false;
        loadBlock();
        changed();
    });
    connect(m_blockText, &QPlainTextEdit::textChanged, this, &WidgetEditorDialog::updateBlock);
    connect(m_fontSize, &QSpinBox::valueChanged, this, [this]() { updateBlock(); });
    connect(m_align, &QComboBox::currentIndexChanged, this, [this]() { updateBlock(); });
    connect(m_bold, &QCheckBox::toggled, this, [this]() { updateBlock(); });
    connect(m_progress, &QSpinBox::valueChanged, this, [this]() { updateBlock(); });
    connect(refresh, &QPushButton::clicked, this, &WidgetEditorDialog::restartPreview);
    connect(m_undoButton, &QPushButton::clicked, this, &WidgetEditorDialog::undoEdit);
    connect(m_redoButton, &QPushButton::clicked, this, &WidgetEditorDialog::redoEdit);
    connect(m_saveButton, &QPushButton::clicked, this, [this]() { saveDraft(); });
    connect(addDesktop, &QPushButton::clicked, this, [this]() {
        if (!saveDraft()) return;
        QScreen* screen = QApplication::screenAt(frameGeometry().center());
        if (!screen) screen = QApplication::primaryScreen();
        if (!DesktopWidget::addCustomWidget(m_draft, screen->availableGeometry().topLeft() + QPoint(32, 32)))
            m_status->setText(QStringLiteral("已保存，桌面空间不足。请缩小尺寸或移除其他组件后重试。"));
        else m_status->setText(QStringLiteral("已添加到桌面。"));
    });
    connect(done, &QPushButton::clicked, this, &QDialog::close);
    auto shortcut = [this](const QKeySequence& sequence, auto action) {
        auto* key = new QShortcut(sequence, this);
        key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(key, &QShortcut::activated, this, action);
    };
    shortcut(QKeySequence::Save, [this]() { saveDraft(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), [this]() { undoEdit(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this]() { redoEdit(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this]() { redoEdit(); });
    for (auto* field : findChildren<QLineEdit*>()) field->installEventFilter(this);
    for (auto* field : findChildren<QPlainTextEdit*>()) field->installEventFilter(this);
    refreshCatalog();
    loadDraft(m_definitions.isEmpty() ? CustomWidgets::makeTemplate() : m_definitions.first());
    refreshCatalog(m_draft.id);
}

void WidgetEditorDialog::refreshCatalog(const QString& selectedId) {
    m_catalogLoading = true;
    QString error;
    m_definitions = CustomWidgets::load(&error);
    m_catalog->clear();
    m_catalog->addItem(QStringLiteral("新组件（未保存）"), QString());
    int selected = 0;
    for (const auto& definition : m_definitions) {
        m_catalog->addItem(definition.name, definition.id);
        if (definition.id == selectedId) selected = m_catalog->count() - 1;
    }
    m_catalog->setCurrentIndex(selected);
    m_catalogLoading = false;
    updateActions();
    if (!error.isEmpty()) m_status->setText(error);
}

void WidgetEditorDialog::loadDraft(const CustomWidgets::Definition& definition) {
    m_previewTimer.stop();
    m_loading = true;
    m_draft = definition;
    m_name->setText(definition.name);
    m_mode->setCurrentIndex(definition.mode == "html" ? 1 : 0);
    m_pages->setCurrentIndex(m_mode->currentIndex());
    m_size->setCurrentIndex(definition.variant);
    m_background->setText(definition.background);
    m_foreground->setText(definition.foreground);
    m_accent->setText(definition.accent);
    m_padding->setValue(definition.padding);
    m_network->setChecked(definition.allowNetwork);
    m_html->setPlainText(definition.html);
    m_css->setPlainText(definition.css);
    m_script->setPlainText(definition.javascript);
    m_blocks->clear();
    for (const auto& block : definition.blocks) m_blocks->addItem(blockName(block));
    m_blocks->setCurrentRow(definition.blocks.isEmpty() ? -1 : 0);
    m_loading = false;
    loadBlock();
    if (!m_restoringHistory) {
        m_baseline = snapshot(collect());
        m_history = {collect()};
        m_historyIndex = 0;
    }
    updateActions();
    m_status->setText(QStringLiteral("编辑后保存，或直接保存并添加到桌面。"));
    updatePreview();
}

CustomWidgets::Definition WidgetEditorDialog::collect() const {
    auto definition = m_draft;
    definition.name = m_name->text().trimmed();
    definition.mode = m_mode->currentIndex() == 1 ? "html" : "blocks";
    definition.variant = m_size->currentIndex();
    definition.background = m_background->text();
    definition.foreground = m_foreground->text();
    definition.accent = m_accent->text();
    definition.padding = m_padding->value();
    definition.allowNetwork = m_network->isChecked();
    definition.html = m_html->toPlainText();
    definition.css = m_css->toPlainText();
    definition.javascript = m_script->toPlainText();
    return definition;
}

bool WidgetEditorDialog::dirty() const { return snapshot(collect()) != m_baseline; }

void WidgetEditorDialog::changed() {
    if (m_loading) return;
    const auto definition = collect();
    if (!m_restoringHistory && (m_historyIndex < 0 || snapshot(m_history[m_historyIndex]) != snapshot(definition))) {
        while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
        m_history.append(definition);
        if (m_history.size() > 60) m_history.removeFirst();
        m_historyIndex = m_history.size() - 1;
    }
    updateActions();
    m_status->setText(dirty() ? QStringLiteral("有未保存的修改") : QStringLiteral("已保存"));
    m_previewTimer.start();
}

void WidgetEditorDialog::updateActions() {
    if (!m_name || !m_undoButton || !m_redoButton) return;
    setWindowTitle(QStringLiteral("小组件工作室 · ") + m_name->text() + (dirty() ? " *" : ""));
    m_undoButton->setEnabled(m_historyIndex > 0);
    m_redoButton->setEnabled(m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size());
    bool saved = false;
    for (const auto& definition : m_definitions)
        if (definition.id == m_draft.id) { saved = true; break; }
    m_deleteAction->setEnabled(saved);
    const int row = m_blocks->currentRow();
    const bool selected = row >= 0 && row < m_draft.blocks.size();
    m_upButton->setEnabled(selected && row > 0);
    m_downButton->setEnabled(selected && row + 1 < m_draft.blocks.size());
    m_duplicateBlockButton->setEnabled(selected && m_draft.blocks.size() < 40);
    m_deleteBlockButton->setEnabled(selected);
}

void WidgetEditorDialog::loadBlock() {
    if (m_loading) return;
    const int row = m_blocks->currentRow();
    const bool valid = row >= 0 && row < m_draft.blocks.size();
    m_loading = true;
    const QJsonObject block = valid ? m_draft.blocks[row] : QJsonObject();
    const QString type = block.value("type").toString();
    const bool hasText = valid && (type == "text" || type == "metric" || type == "progress");
    const bool hasFont = valid && type != "divider";
    m_blockForm->setRowVisible(m_blockText, hasText);
    m_blockForm->setRowVisible(m_fontSize, hasFont);
    m_blockForm->setRowVisible(m_align, hasFont);
    m_blockForm->setRowVisible(m_bold, hasFont);
    m_blockForm->setRowVisible(m_progress, valid && type == "progress");
    m_blockText->setPlainText(block.value("text").toString());
    m_fontSize->setValue(block.value("size").toInt(18));
    m_align->setCurrentIndex(qMax(0, QStringList{"left", "center", "right"}.indexOf(block.value("align").toString("left"))));
    m_bold->setChecked(block.value("bold").toBool(type == "metric"));
    m_progress->setValue(block.value("value").toInt(50));
    m_blockHint->setText(!valid ? QStringLiteral("选择内容块进行编辑，或在上方添加新内容。")
        : type == "divider" ? QStringLiteral("分隔线上下内容，无需其他设置。")
        : type == "clock" ? QStringLiteral("自动显示当前时间。")
        : type == "date" ? QStringLiteral("自动显示当前日期。") : QString());
    m_blockHint->setVisible(!m_blockHint->text().isEmpty());
    m_loading = false;
    updateActions();
}

void WidgetEditorDialog::updateBlock() {
    if (m_loading) return;
    const int row = m_blocks->currentRow();
    if (row < 0 || row >= m_draft.blocks.size()) return;
    auto& block = m_draft.blocks[row];
    const QString type = block.value("type").toString();
    if (type == "text" || type == "metric" || type == "progress") block.insert("text", m_blockText->toPlainText());
    if (type != "divider") {
        block.insert("size", m_fontSize->value());
        block.insert("align", QStringList{"left", "center", "right"}[qMax(0, m_align->currentIndex())]);
        block.insert("bold", m_bold->isChecked());
    }
    if (type == "progress") block.insert("value", m_progress->value());
    m_blocks->item(row)->setText(blockName(block));
    changed();
}

void WidgetEditorDialog::undoEdit() { restoreHistory(m_historyIndex - 1); }
void WidgetEditorDialog::redoEdit() { restoreHistory(m_historyIndex + 1); }

void WidgetEditorDialog::restoreHistory(int index) {
    if (index < 0 || index >= m_history.size() || index == m_historyIndex) return;
    const int selectedBlock = m_blocks->currentRow();
    m_restoringHistory = true;
    m_historyIndex = index;
    loadDraft(m_history[index]);
    m_blocks->setCurrentRow(qMin(selectedBlock, m_blocks->count() - 1));
    m_restoringHistory = false;
    updateActions();
    m_status->setText(dirty() ? QStringLiteral("有未保存的修改") : QStringLiteral("已保存"));
}

void WidgetEditorDialog::updatePreview() {
    auto definition = collect();
    CustomWidgets::Definition checked;
    QString error;
    if (!CustomWidgets::fromJson(definition.toJson(), checked, &error)) { m_status->setText(error); return; }
    const qreal scale = qBound<qreal>(.4, AppSettings().value("appearance/scale", 1.0).toDouble(), 2);
    QScreen* screen = QApplication::screenAt(frameGeometry().center());
    if (!screen) screen = QApplication::primaryScreen();
    const QSize size = DesktopWidget::cardSize(DesktopWidget::CardKind::Custom, definition.variant, scale, screen->availableGeometry());
    const QSize content(qMax(32, size.width() - qRound(16 * scale)), qMax(32, size.height() - qRound(42 * scale)));
    const qreal fit = qBound<qreal>(.05, qMin(qreal(qMax(16, m_previewArea->viewport()->width() - 24)) / content.width(),
        qreal(qMax(16, m_previewArea->viewport()->height() - 24)) / content.height()), 1);
    m_preview->setFixedSize(content * fit);
    m_preview->setContentScale(scale * fit);
    definition.name = QStringLiteral("小组件预览");
    const QString document = CustomWidgets::sandboxedDocument(definition);
    if (document != m_previewDocument) {
        m_previewDocument = document;
        m_preview->setDocument(document);
    }
    m_previewSize->setText(QStringLiteral("桌面 %1 × %2 · 预览 %3%").arg(size.width()).arg(size.height()).arg(qRound(fit * 100)));
}

void WidgetEditorDialog::previewStatusChanged(const QString& status) {
    if (!m_preview->errorText().isEmpty()) {
        m_previewStatus = status + QStringLiteral("，可点击“重载”重试。");
        m_status->setText(m_previewStatus);
    } else if (!m_previewStatus.isEmpty() && m_status->text() == m_previewStatus) {
        m_status->setText(dirty() ? QStringLiteral("有未保存的修改") : QStringLiteral("预览已更新"));
        m_previewStatus.clear();
    }
}

void WidgetEditorDialog::restartPreview() {
    m_previewTimer.stop();
    auto* host = m_previewArea->widget();
    host->layout()->removeWidget(m_preview);
    delete m_preview;
    m_preview = new HtmlWidgetView;
    m_preview->setObjectName("customPreview");
    qobject_cast<QVBoxLayout*>(host->layout())->addWidget(m_preview, 0, Qt::AlignCenter);
    m_preview->statusChanged = [this](const QString& status) {
        previewStatusChanged(status);
    };
    m_previewDocument.clear();
    m_previewStatus = QStringLiteral("正在重新加载预览…");
    m_status->setText(m_previewStatus);
    updatePreview();
}

bool WidgetEditorDialog::saveDraft() {
    const auto definition = collect();
    QString error;
    if (!DesktopWidget::saveCustomDefinition(definition, &error)) { m_status->setText(error); return false; }
    m_draft = definition;
    m_baseline = snapshot(definition);
    refreshCatalog(definition.id);
    updateActions();
    m_status->setText(QStringLiteral("已保存到组件库，桌面实例已同步更新。"));
    return true;
}

bool WidgetEditorDialog::confirmDiscard() {
    if (!dirty()) return true;
    QMessageBox box(QMessageBox::Question, QStringLiteral("保存修改"), QStringLiteral("当前组件有未保存的修改。"), QMessageBox::NoButton, this);
    auto* save = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    auto* discard = box.addButton(QStringLiteral("放弃修改"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == save) return saveDraft();
    return box.clickedButton() == discard;
}

bool WidgetEditorDialog::selectDefinition(const CustomWidgets::Definition& definition) {
    if (m_draft.id == definition.id) return true;
    if (!confirmDiscard()) return false;
    loadDraft(definition);
    refreshCatalog(definition.id);
    return true;
}

void WidgetEditorDialog::newDraft(int templateIndex) {
    if (!confirmDiscard()) return;
    loadDraft(CustomWidgets::makeTemplate(templateIndex));
    refreshCatalog();
    m_baseline.clear();
    changed();
}

void WidgetEditorDialog::duplicateDraft() {
    auto copy = collect();
    if (!confirmDiscard()) return;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = copy.name.left(75) + QStringLiteral(" 副本");
    loadDraft(copy);
    refreshCatalog();
    m_baseline.clear();
    changed();
}

bool WidgetEditorDialog::importFromFile(const QString& path) {
    CustomWidgets::Definition definition;
    QString error;
    if (!CustomWidgets::importFile(path, definition, &error)) { m_status->setText(error); return false; }
    if (!confirmDiscard()) return false;
    if (!DesktopWidget::saveCustomDefinition(definition, &error)) { m_status->setText(error); return false; }
    loadDraft(definition);
    refreshCatalog(definition.id);
    m_status->setText(QStringLiteral("已导入到组件库，可直接添加到桌面。"));
    return true;
}

void WidgetEditorDialog::importDraft() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入组件"), {}, QStringLiteral("组件文件 (*.json *.html *.htm)"));
    if (!path.isEmpty()) importFromFile(path);
}

void WidgetEditorDialog::exportDraft() {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出组件"),
        m_name->text().trimmed().replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_") + ".json", QStringLiteral("组件文件 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    m_status->setText(CustomWidgets::exportFile(path, collect(), &error) ? QStringLiteral("组件已导出，可在其他电脑导入。") : error);
}

void WidgetEditorDialog::deleteDefinition() {
    bool exists = false;
    for (const auto& definition : m_definitions) if (definition.id == m_draft.id) exists = true;
    if (!exists) { m_status->setText(QStringLiteral("当前组件尚未保存到组件库。")); return; }
    QMessageBox box(QMessageBox::Question, QStringLiteral("删除组件"),
        QStringLiteral("从组件库删除“%1”？桌面现有实例保留，当前未保存的修改将被放弃。").arg(m_draft.name), QMessageBox::NoButton, this);
    auto* remove = box.addButton(QStringLiteral("删除"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != remove) return;
    QString error;
    if (!CustomWidgets::remove(m_draft.id, &error)) { m_status->setText(error); return; }
    WidgetLibraryDialog::refreshCustomWidgets();
    refreshCatalog();
    loadDraft(m_definitions.isEmpty() ? CustomWidgets::makeTemplate() : m_definitions.first());
    refreshCatalog(m_draft.id);
    m_status->setText(QStringLiteral("已从组件库删除。"));
}

void WidgetEditorDialog::reject() { if (confirmDiscard()) QDialog::reject(); }

void WidgetEditorDialog::closeEvent(QCloseEvent* event) {
    if (!confirmDiscard()) { event->ignore(); return; }
    m_baseline = snapshot(collect());
    QDialog::closeEvent(event);
}

bool WidgetEditorDialog::eventFilter(QObject* watched, QEvent* event) {
    if (m_previewArea && watched == m_previewArea->viewport() && event->type() == QEvent::Resize && m_preview)
        m_previewTimer.start();
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Undo)) { undoEdit(); return true; }
        if (key->matches(QKeySequence::Redo)) { redoEdit(); return true; }
    }
    return QDialog::eventFilter(watched, event);
}
