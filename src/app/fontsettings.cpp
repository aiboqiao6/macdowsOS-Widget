#include "app/fontsettings.h"
#include "app/appsettings.h"
#include "rendering/cleartypetext.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QWidget>

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

}

QString family()
{
    const QString family = pingFangFamily();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
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
