#pragma once
#include "widgets/customwidget.h"
#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QFormLayout;
class QPushButton;
class QSplitter;
class QAction;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QScrollArea;
class HtmlWidgetView;

class WidgetEditorDialog final : public QDialog {
public:
    explicit WidgetEditorDialog(QWidget* parent = nullptr);
    bool selectDefinition(const CustomWidgets::Definition& definition);
protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    friend class WidgetRegression;
    void refreshCatalog(const QString& selectedId = {});
    void loadDraft(const CustomWidgets::Definition& definition);
    CustomWidgets::Definition collect() const;
    bool saveDraft();
    bool confirmDiscard();
    bool dirty() const;
    void changed();
    void updatePreview();
    void previewStatusChanged(const QString& status);
    void loadBlock();
    void updateBlock();
    void newDraft(int templateIndex = 0);
    void duplicateDraft();
    void undoEdit();
    void redoEdit();
    void updateActions();
    void restoreHistory(int index);
    void restartPreview();
    bool importFromFile(const QString& path);
    void importDraft();
    void exportDraft();
    void deleteDefinition();
    CustomWidgets::Definition m_draft;
    QByteArray m_baseline;
    QList<CustomWidgets::Definition> m_definitions;
    bool m_loading = false;
    bool m_catalogLoading = false;
    QComboBox* m_catalog = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_size = nullptr;
    QLineEdit* m_background = nullptr;
    QLineEdit* m_foreground = nullptr;
    QLineEdit* m_accent = nullptr;
    QSpinBox* m_padding = nullptr;
    QCheckBox* m_network = nullptr;
    QStackedWidget* m_pages = nullptr;
    QListWidget* m_blocks = nullptr;
    QComboBox* m_blockType = nullptr;
    QPlainTextEdit* m_blockText = nullptr;
    QSpinBox* m_fontSize = nullptr;
    QComboBox* m_align = nullptr;
    QCheckBox* m_bold = nullptr;
    QSpinBox* m_progress = nullptr;
    QFormLayout* m_blockForm = nullptr;
    QWidget* m_stylePanel = nullptr;
    QLabel* m_blockHint = nullptr;
    QPushButton* m_undoButton = nullptr;
    QPushButton* m_redoButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_upButton = nullptr;
    QPushButton* m_downButton = nullptr;
    QPushButton* m_duplicateBlockButton = nullptr;
    QPushButton* m_deleteBlockButton = nullptr;
    QSplitter* m_splitter = nullptr;
    QAction* m_deleteAction = nullptr;
    QPlainTextEdit* m_html = nullptr;
    QPlainTextEdit* m_css = nullptr;
    QPlainTextEdit* m_script = nullptr;
    HtmlWidgetView* m_preview = nullptr;
    QScrollArea* m_previewArea = nullptr;
    QLabel* m_previewSize = nullptr;
    QLabel* m_status = nullptr;
    QString m_previewDocument;
    QString m_previewStatus;
    QList<CustomWidgets::Definition> m_history;
    int m_historyIndex = -1;
    bool m_restoringHistory = false;
    QTimer m_previewTimer;
};
