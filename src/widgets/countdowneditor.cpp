#include "ui/textcontrols.h"
#include "widgets/desktopwidget.h"
#include <QCalendarWidget>
#include <QCheckBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

void DesktopWidget::editCountdown()
{
    if (m_cardKind != CardKind::Countdown)
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("countdownEditor"));
    dialog.setWindowTitle(QStringLiteral("编辑倒数日"));
    dialog.setModal(true);
    QFont font(pingFangFontFamily());
    font.setPointSizeF(10.5);
    applyFontSmoothing(font);
    dialog.setFont(font);
    const bool dark = darkModeEnabled();
    const QString background = dark ? QStringLiteral("#202126") : QStringLiteral("#f5f5f7");
    const QString foreground = dark ? QStringLiteral("#f5f5f7") : QStringLiteral("#202124");
    const QString field = dark ? QStringLiteral("#303137") : QStringLiteral("#ffffff");
    const QString border = dark ? QStringLiteral("#54555e") : QStringLiteral("#d5d6dc");
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:%1; color:%2; } QLabel, QCheckBox { color:%2; }"
        "QLineEdit, QDateEdit { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:8px; }"
        "QCalendarWidget QWidget { background:%3; color:%2; }"
        "QCalendarWidget QAbstractItemView { selection-background-color:#fa586a; selection-color:white; }"
        "QPushButton { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:7px 18px; }"
        "QPushButton:default { background:#fa586a; color:white; border-color:#fa586a; }")
        .arg(background, foreground, field, border));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(18);
    auto* heading = new AppText::Label(QStringLiteral("记住一个重要的日子"), &dialog);
    heading->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600;"));
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    form->setSpacing(12);
    auto* title = new AppText::LineEdit(m_countdown.title, &dialog);
    title->setObjectName(QStringLiteral("countdownTitle"));
    title->setMaxLength(80);
    title->setPlaceholderText(QStringLiteral("例如：生日、旅行、毕业"));
    auto* date = new AppText::DateEdit(&dialog);
    date->setObjectName(QStringLiteral("countdownDate"));
    date->setDateRange(QDate(1900, 1, 1), QDate(9999, 12, 31));
    date->setDate(m_countdown.date);
    date->setDisplayFormat(QStringLiteral("yyyy 年 MM 月 dd 日"));
    date->setCalendarPopup(true);
    date->calendarWidget()->setFirstDayOfWeek(Qt::Monday);
    auto* annual = new QCheckBox(QStringLiteral("每年重复这个纪念日"), &dialog);
    annual->setObjectName(QStringLiteral("countdownAnnual"));
    annual->setChecked(m_countdown.annual);
    form->addRow(QStringLiteral("标题"), title);
    form->addRow(QStringLiteral("日期"), date);
    form->addRow(QString(), annual);
    layout->addLayout(form);
    auto* preview = new AppText::Label(&dialog);
    preview->setObjectName(QStringLiteral("countdownSummary"));
    preview->setStyleSheet(QStringLiteral("font-size:16px; color:#fa586a;"));
    layout->addWidget(preview);
    auto* help = new AppText::Label(QStringLiteral("每年自动计算下一次纪念日。2 月 29 日在平年按 2 月 28 日计算。"), &dialog);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(dark ? QStringLiteral("#a4a5b0") : QStringLiteral("#72737d")));
    help->setVisible(annual->isChecked());
    layout->addWidget(help);
    const auto updateSummary = [=] {
        const CountdownCard::State state = CountdownCard::state({title->text(), date->date(), annual->isChecked()});
        preview->setText(state.days == 0 ? QStringLiteral("就是今天")
            : QStringLiteral("%1 %2 天").arg(state.elapsed ? QStringLiteral("已经过去") : QStringLiteral("还有")).arg(state.days));
    };
    connect(date, &QDateEdit::dateChanged, &dialog, updateSummary);
    connect(annual, &QCheckBox::toggled, &dialog, [=](bool checked) {
        help->setVisible(checked);
        updateSummary();
    });
    updateSummary();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(420, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_countdown.title = title->text().simplified();
    if (m_countdown.title.isEmpty())
        m_countdown.title = QStringLiteral("值得期待的一天");
    m_countdown.date = date->date();
    m_countdown.annual = annual->isChecked();
    m_countdownLastDate = QDate::currentDate();
    update();
    saveConfiguration();
}
