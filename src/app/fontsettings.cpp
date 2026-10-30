#include "app/fontsettings.h"
#include "app/appsettings.h"
#include "rendering/cleartypetext.h"
#include "rendering/responsivelayout.h"
#include "rendering/textpaintdevice.h"
#include "ui/textcontrols.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QWidget>
#include <QProxyStyle>
#include <QPointer>
#include <QTextLayout>
#include <QtEndian>
#include <algorithm>

namespace AppFonts {
namespace {
int s_fontSmoothingLevel = 4;

bool usesInterfaceText(const QWidget* widget)
{
    // Walk parents as combo popups and tool windows also belong to the interface.
    for (auto* current = widget; current; current = current->parentWidget())
        if (current->property("independentInterfaceText").toBool()) return true;
    return false;
}

void configureInterfaceText(QFont& font)
{
    font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        QFont::PreferAntialias | QFont::PreferQuality | QFont::NoSubpixelAntialias));
    font.setHintingPreference(QFont::PreferNoHinting);
}

void configureFontSmoothing(QFont& font, int level)
{
    // macOS-style grayscale coverage: no RGB fringing or Windows grid fitting.
    if (level == 0) {
        font.setStyleStrategy(QFont::NoAntialias);
    } else configureInterfaceText(font);
    font.setHintingPreference(QFont::PreferNoHinting);
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

    void polish(QWidget* widget) override
    {
        QProxyStyle::polish(widget);
        if (auto* area = qobject_cast<QAbstractScrollArea*>(widget))
            AppText::installViewport(area);
    }

    void drawControl(ControlElement element, const QStyleOption* option,
                     QPainter* painter, const QWidget* widget = nullptr) const override
    {
        if (!painter || AppText::PaintDevice::forPainter(*painter)) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        AppText::PaintDevice device(*painter);
        QPainter custom(&device);
        custom.setFont(painter->font());
        QProxyStyle::drawControl(element, option, &custom, widget);
    }

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
        // Keep settings chrome legible independently of widget quality modes.
        const bool interface = usesInterfaceText(dynamic_cast<QWidget*>(painter->device()));
        if (interface) configureInterfaceText(font);
        else if (!(font.styleStrategy() & QFont::NoSubpixelAntialias))
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
            ResponsiveLayout::drawHighQualityText(*painter, baseline, part);
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
    ClearTypeText::setEnabled(true);
    AppSettings().setValue(QStringLiteral("appearance/fontSmoothing"), next);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont font = app->font();
        font.setFamily(pingFangFamily());
        configureFontSmoothing(font, next);
        app->setFont(font);
        QList<QPointer<QWidget>> widgets;
        for (QWidget* widget : app->allWidgets()) widgets.append(widget);
        for (const auto& guarded : widgets) {
            QWidget* widget = guarded.data();
            if (!widget) continue;
            // 应用字体更新后，显式刷新现有控件解析后的策略，避免 QSS 保留旧字体配置。
            QFont widgetFont = widget->font();
            widgetFont.setFamily(pingFangFamily());
            if (usesInterfaceText(widget)) configureInterfaceText(widgetFont);
            else configureFontSmoothing(widgetFont, next);
            if (widgetFont != widget->font()) widget->setFont(widgetFont);
            if (!usesInterfaceText(widget)) widget->update();
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
    font.setFamily(pingFangFamily());
    configureFontSmoothing(font, s_fontSmoothingLevel);
}

void useInterfaceText(QWidget* window)
{
    window->setProperty("independentInterfaceText", true);
    QFont font = window->font();
    font.setFamily(family());
    configureInterfaceText(font);
    if (font != window->font()) window->setFont(font);
    QList<QPointer<QWidget>> children;
    for (auto* child : window->findChildren<QWidget*>()) children.append(child);
    for (const auto& guarded : children) {
        auto* child = guarded.data();
        if (!child) continue;
        child->ensurePolished();
        if (!guarded) continue;
        QFont childFont = child->font();
        childFont.setFamily(pingFangFamily());
        configureInterfaceText(childFont);
        if (childFont != child->font()) child->setFont(childFont);
    }
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

QString webFontUrl()
{
    return QStringLiteral("https://macdows-fonts.invalid/pingfang.ttf");
}

QByteArray webFontData()
{
    QFile file(QStringLiteral(":/fonts/pingfang0.ttf"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    if (bytes.size() < 12) return {};
    const auto read16 = [&](qsizetype at) { return qFromBigEndian<quint16>(bytes.constData() + at); };
    const auto read32 = [&](qsizetype at) { return qFromBigEndian<quint32>(bytes.constData() + at); };
    const auto write16 = [&](qsizetype at, quint16 value) { qToBigEndian(value, bytes.data() + at); };
    const auto write32 = [&](qsizetype at, quint32 value) { qToBigEndian(value, bytes.data() + at); };
    const int tables = read16(4);
    if (read32(0) != 0x00010000 || 12 + tables * 16 > bytes.size()) return {};
    const auto checksum = [&](qsizetype offset, qsizetype length) {
        quint32 sum = 0;
        for (qsizetype i = 0; i < length; i += 4) {
            quint32 word = 0;
            for (int byte = 0; byte < 4; ++byte)
                word = (word << 8) | (i + byte < length ? quint8(bytes[offset + i + byte]) : 0);
            sum += word;
        }
        return sum;
    };
    int retained = 0;
    qsizetype head = -1;
    for (int i = 0; i < tables; ++i) {
        const qsizetype entry = 12 + i * 16;
        const QByteArray tag = bytes.mid(entry, 4);
        const quint32 offset = read32(entry + 8), length = read32(entry + 12);
        if (quint64(offset) + length > quint64(bytes.size())) return {};
        // This bundled conversion has truncated vertical metrics and maxZones=0.
        // Browser OTS rejects those metadata tables although horizontal glyphs
        // are valid. Preserve outlines/advances; let the browser synthesize vertical metrics.
        if (tag == "vhea" || tag == "vmtx") continue;
        if (tag == "maxp" && length >= 32 && read16(offset + 14) == 0)
            write16(offset + 14, 2);
        if (tag == "head" && length >= 12) { head = offset; write32(head + 8, 0); }
        const qsizetype destination = 12 + retained++ * 16;
        const QByteArray record = bytes.mid(entry, 16);
        std::copy(record.begin(), record.end(), bytes.begin() + destination);
        write32(destination + 4, checksum(offset, length));
    }
    if (head < 0 || !retained) return {};
    std::fill(bytes.begin() + 12 + retained * 16, bytes.begin() + 12 + tables * 16, char(0));
    int power = 1, selector = 0;
    while (power * 2 <= retained) { power *= 2; ++selector; }
    write16(4, quint16(retained));
    write16(6, quint16(power * 16));
    write16(8, quint16(selector));
    write16(10, quint16(retained * 16 - power * 16));
    write32(head + 8, quint32(0xb1b0afbaU - checksum(0, bytes.size())));
    return bytes;
}

QString webStyleSheet()
{
    // WebView2 is a separate process and cannot see QFontDatabase's private font.
    // Serve the bundled face locally rather than relying on an installed font.
    return QStringLiteral("@font-face{font-family:'macdows PingFang';src:url('%1') format('truetype');font-display:block;}"
        "html,body,body *,input,textarea,select,button{font-family:'macdows PingFang',sans-serif!important;"
        "-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility;}").arg(webFontUrl());
}
}
