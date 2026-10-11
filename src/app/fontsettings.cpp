#include "app/fontsettings.h"
#include "app/appsettings.h"
#include "rendering/cleartypetext.h"
#include "rendering/responsivelayout.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QWidget>
#include <QProxyStyle>
#include <QTextLayout>

namespace AppFonts {
namespace {
int s_fontSmoothingLevel = 4;

void configureFontSmoothing(QFont& font, int level)
{
    switch (qBound(0, level, 4)) {
    case 0:
        font.setStyleStrategy(QFont::NoAntialias);
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    case 1:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::NoSubpixelAntialias));
        font.setHintingPreference(QFont::PreferVerticalHinting);
        break;
    case 3:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::PreferQuality | QFont::NoSubpixelAntialias));
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    case 2:
    default:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::PreferQuality));
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    }
}

QString pingFangFamily()
{
    static const QString family = [] {
        const QStringList candidates = {
            QStringLiteral(":/fonts/pingfang0.ttf"),
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("pingfang0.ttf")),
            QDir::current().filePath(QStringLiteral("pingfang0.ttf"))
        };
        for (const QString& path : candidates) {
            if (!QFile::exists(path))
                continue;
            const int id = QFontDatabase::addApplicationFont(path);
            if (id < 0)
                continue;
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty())
                return families.first();
        }
        // 资源不可用时仍使用字体内嵌族名，由 Qt 查找已安装的同名字体。
        return QStringLiteral("PingFang SC");
    }();
    return family;
}

class UnifiedTextStyle final : public QProxyStyle
{
public:
    explicit UnifiedTextStyle(QStyle* base) : QProxyStyle(base) {}

    void drawItemText(QPainter* painter, const QRect& rect, int flags,
                      const QPalette& palette, bool enabled, const QString& source,
                      QPalette::ColorRole role = QPalette::NoRole) const override
    {
        if (!painter || source.isEmpty())
            return;
        QString text = source;
        int mnemonic = -1;
        if (flags & (Qt::TextShowMnemonic | Qt::TextHideMnemonic)) {
            text.clear();
            for (int i = 0; i < source.size(); ++i) {
                if (source[i] == QLatin1Char('&') && i + 1 < source.size()) {
                    if (source[i + 1] == QLatin1Char('&')) ++i;
                    else { mnemonic = text.size(); continue; }
                }
                text += source[i];
            }
        }
        painter->save();
        QFont font = painter->font();
        font.setFamily(pingFangFamily());
        configureFontSmoothing(font, s_fontSmoothingLevel);
        painter->setFont(font);
        if (role != QPalette::NoRole)
            painter->setPen(palette.color(enabled ? palette.currentColorGroup() : QPalette::Disabled, role));
        if (!(flags & Qt::TextDontClip))
            painter->setClipRect(rect, Qt::IntersectClip);
        text.replace(QLatin1Char('\n'), flags & Qt::TextSingleLine ? QChar(' ') : QChar(QChar::LineSeparator));
        QTextLayout layout(text, font);
        QTextOption option;
        option.setWrapMode(flags & Qt::TextWordWrap ? QTextOption::WordWrap : QTextOption::NoWrap);
        option.setTextDirection(painter->layoutDirection());
        layout.setTextOption(option);
        layout.beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(qMax(1, rect.width()));
            line.setPosition(QPointF(0, height));
            height += line.height();
        }
        layout.endLayout();
        qreal top = rect.top();
        if (flags & Qt::AlignVCenter) top += (rect.height() - height) / 2;
        else if (flags & Qt::AlignBottom) top += rect.height() - height;
        for (int i = 0; i < layout.lineCount(); ++i) {
            const QTextLine line = layout.lineAt(i);
            qreal left = rect.left();
            if (flags & Qt::AlignHCenter) left += (rect.width() - line.naturalTextWidth()) / 2;
            else if (flags & Qt::AlignRight) left += rect.width() - line.naturalTextWidth();
            const QPointF baseline(left, top + line.y() + line.ascent());
            QString part = text.mid(line.textStart(), line.textLength());
            part.remove(QChar::LineSeparator);
            if (!ResponsiveLayout::drawHighQualityText(*painter, baseline, part))
                painter->drawText(baseline, part);
            if (mnemonic >= line.textStart() && mnemonic < line.textStart() + line.textLength()
                && (flags & Qt::TextShowMnemonic) && !(flags & Qt::TextHideMnemonic)) {
                const QFontMetricsF metrics(font);
                const int index = mnemonic - line.textStart();
                const qreal x = baseline.x() + metrics.horizontalAdvance(part.left(index));
                painter->drawLine(QPointF(x, baseline.y() + metrics.underlinePos()),
                    QPointF(x + metrics.horizontalAdvance(part.mid(index, 1)), baseline.y() + metrics.underlinePos()));
            }
        }
        painter->restore();
    }
};

}

QString family()
{
    const QString family = pingFangFamily();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        static bool styleInstalled = false;
        if (!styleInstalled) {
            styleInstalled = true;
            app->setStyle(new UnifiedTextStyle(app->style()));
        }
        QFont applicationFont = app->font();
        if (applicationFont.family() != family) {
            applicationFont.setFamily(family);
            configureFontSmoothing(applicationFont, s_fontSmoothingLevel);
            app->setFont(applicationFont);
        }
    }
    return family;
}

bool setSmoothing(int level)
{
    const int next = qBound(0, level, 4);
    const bool changed = s_fontSmoothingLevel != next;
    s_fontSmoothingLevel = next;
    ClearTypeText::setEnabled(next == 4);
    AppSettings().setValue(QStringLiteral("appearance/fontSmoothing"), next);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont font = app->font();
        font.setFamily(pingFangFamily());
        configureFontSmoothing(font, next);
        app->setFont(font);
        for (QWidget* widget : app->allWidgets()) {
            // 应用字体更新后，显式刷新现有控件解析后的策略，避免 QSS 保留旧字体配置。
            QFont widgetFont = widget->font();
            configureFontSmoothing(widgetFont, next);
            widget->setFont(widgetFont);
            widget->update();
        }
    }
    return changed;
}

int smoothing()
{
    return s_fontSmoothingLevel;
}

void applySmoothing(QFont& font)
{
    configureFontSmoothing(font, s_fontSmoothingLevel);
}

QFont displayFont(int pointSize, QFont::Weight weight)
{
    QFont font;
    font.setFamily(family());
    font.setPointSize(pointSize);
    font.setWeight(weight);
    applySmoothing(font);
    return font;
}
}
