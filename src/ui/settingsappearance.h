#pragma once

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QWidget>
#include <cmath>

// Theme values are em units or ratios, never device pixels. Qt applies screen DPI.
// Each dialog takes a snapshot so an override cannot change half a layout mid-paint.
namespace SettingsAppearance {
inline QVariantMap values(const QWidget* window)
{
    return window->property("settingsTheme").toMap();
}
inline qreal number(const QWidget* window, const char* name)
{
    return values(window).value(QString::fromLatin1(name)).toDouble();
}
inline qreal em(const QWidget* window)
{
    const auto font = window->font();
    return font.pointSizeF() > 0 ? font.pointSizeF() * window->logicalDpiY() / 72.
        : QFontInfo(font).pixelSize();
}
inline int px(const QWidget* window, const char* name)
{
    return qRound(em(window) * number(window, name));
}
inline QColor color(const QWidget* window, const char* name)
{
    const auto palette = window->property(window->property("settingsDark").toBool()
        ? "settingsDarkColors" : "settingsLightColors").toHash();
    return palette.value(QString::fromLatin1(name)).value<QColor>();
}
inline void load(QWidget* window)
{
    QFile defaults(QStringLiteral(":/settings/theme.json"));
    if (!defaults.open(QIODevice::ReadOnly)) qFatal("Missing settings theme resource");
    QJsonObject theme = QJsonDocument::fromJson(defaults.readAll()).object();
    QString overridePath = qEnvironmentVariable("MACDOWSOS_SETTINGS_THEME");
    if (overridePath.isEmpty())
        overridePath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/settings-theme.json");
    QFile custom(overridePath);
    if (custom.exists() && custom.open(QIODevice::ReadOnly)) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(custom.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning("Settings theme override is invalid; keeping bundled theme");
        } else {
            const auto overrides = document.object();
            for (auto it = overrides.begin(); it != overrides.end(); ++it) {
                if (!theme.contains(it.key())) continue;
                if (it.value().isObject() && theme.value(it.key()).isObject()) {
                    auto palette = theme.value(it.key()).toObject();
                    const auto requested = it.value().toObject();
                    for (auto c = requested.begin(); c != requested.end(); ++c)
                        if (palette.contains(c.key()) && c.value().isString()
                            && QColor(c.value().toString()).isValid())
                            palette.insert(c.key(), c.value());
                    theme.insert(it.key(), palette);
                } else if (it.value().isDouble() && theme.value(it.key()).isDouble()) {
                    const double n = it.value().toDouble();
                    // Structural validation, independent of the reference's design values.
                    if (std::isfinite(n) && n >= 0 && n <= 200
                        && (n > 0 || theme.value(it.key()).toDouble() == 0)
                        && (it.key() != QStringLiteral("sidebarRatio") || n < 0.5)
                        && (it.key() != QStringLiteral("sidebarCaptureOverscan") || n <= 1)
                        && (it.key() != QStringLiteral("fontScale") || (n >= 0.5 && n <= 3)))
                        theme.insert(it.key(), n);
                }
            }
        }
    }
    window->setProperty("settingsTheme", theme.toVariantMap());
    // The per-window theme snapshot is immutable. Parse colors once, outside painting.
    for (bool dark : {true, false}) {
        QVariantHash colors;
        const auto palette = theme.value(dark ? QStringLiteral("dark") : QStringLiteral("light")).toObject();
        for (auto it = palette.begin(); it != palette.end(); ++it)
            colors.insert(it.key(), QColor(it.value().toString()));
        window->setProperty(dark ? "settingsDarkColors" : "settingsLightColors", colors);
    }
}
inline void margins(QLayout* layout, const char* left, const char* top,
                    const char* right, const char* bottom, const char* gap = nullptr)
{
    layout->setProperty("themeMargins", QStringList{QString::fromLatin1(left),
        QString::fromLatin1(top), QString::fromLatin1(right), QString::fromLatin1(bottom)});
    if (gap) layout->setProperty("themeGap", QString::fromLatin1(gap));
}
inline void height(QWidget* widget, const char* metric)
{
    widget->setProperty("themeHeight", QString::fromLatin1(metric));
}
inline void square(QWidget* widget, const char* metric)
{
    height(widget, metric);
    widget->setProperty("themeWidth", QString::fromLatin1(metric));
}
inline QString stylesheet(const QWidget* window)
{
    QFile file(QStringLiteral(":/settings/settings.qss"));
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing settings stylesheet resource");
    QString css = QString::fromUtf8(file.readAll());
    QVariantMap tokens = values(window);
    const qreal unit = em(window);
    const auto derived = [&](const char* key, qreal n) {
        tokens.insert(QString::fromLatin1(key), n);
    };
    derived("bodyFont", 1);
    derived("secondaryFont", number(window, "secondaryScale"));
    derived("titleFont", number(window, "titleScale"));
    derived("navigationFont", number(window, "navigationScale"));
    derived("sectionFont", number(window, "sectionScale"));
    derived("historyRadius", std::floor(px(window, "historyHeight") / 2.) / unit);
    derived("searchRadius", std::floor(px(window, "searchHeight") / 2.) / unit);
    derived("selectorSpace", number(window, "selectorDiameter") + number(window, "controlGap"));
    derived("selectorHalf", number(window, "selectorDiameter") / 2);
    derived("scrollRadius", number(window, "scrollWidth") / 2);
    derived("sliderTrackRadius", number(window, "sliderTrack") / 2);
    const int knobMargin = qMax(0, (px(window, "sliderKnob") - px(window, "sliderTrack")) / 2);
    derived("sliderKnobRadius", std::floor(qMin(px(window, "sliderKnob"),
        px(window, "sliderTrack") + knobMargin * 2) / 2.) / unit);
    derived("sliderKnobMargin", knobMargin / unit);
    const QRegularExpression token(QStringLiteral("([@$])([A-Za-z]+)"));
    auto matches = token.globalMatch(css);
    QVector<QRegularExpressionMatch> replacements;
    while (matches.hasNext()) replacements.append(matches.next());
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it) {
        QString value;
        if (it->captured(1) == QStringLiteral("@")) {
            const QColor c = color(window, it->captured(2).toLatin1().constData());
            value = QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
        } else {
            value = QString::number(qMax(0, qRound(tokens.value(it->captured(2)).toDouble() * unit))) + QStringLiteral("px");
        }
        css.replace(it->capturedStart(), it->capturedLength(), value);
    }
    return css;
}
inline void applyMetrics(QWidget* window)
{
    auto widgets = window->findChildren<QWidget*>();
    widgets.prepend(window);
    for (auto* widget : widgets) {
        const auto heightKey = widget->property("themeHeight").toString();
        const auto widthKey = widget->property("themeWidth").toString();
        if (!heightKey.isEmpty()) widget->setFixedHeight(px(window, heightKey.toLatin1().constData()));
        if (!widthKey.isEmpty()) widget->setFixedWidth(px(window, widthKey.toLatin1().constData()));
    }
    for (auto* layout : window->findChildren<QLayout*>()) {
        const auto keys = layout->property("themeMargins").toStringList();
        if (keys.size() == 4) {
            const auto dimension = [&](int i) {return keys[i].isEmpty() ? 0 : px(window, keys[i].toLatin1().constData());};
            layout->setContentsMargins(dimension(0), dimension(1), dimension(2), dimension(3));
        }
        const auto gap = layout->property("themeGap").toString();
        if (!gap.isEmpty()) layout->setSpacing(px(window, gap.toLatin1().constData()));
    }
}
}
