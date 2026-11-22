#pragma once

#include <QColor>
#include <QtGlobal>
#include <algorithm>

// 卡片内容一直按“浅色墨画在深玻璃上”设计；亮色模式把同一批颜色翻成深色墨，
// 让它们落在白色磨砂表面上，而不需要每张卡片维护两套调色板。
// 只翻转界面墨（文字、描边、刻度、轨道）；彩色内容用 accent 保留色相。
namespace CardInk {
inline bool& darkFlag()
{
    static bool dark = true;
    return dark;
}

inline void setDark(bool dark) { darkFlag() = dark; }
inline bool dark() { return darkFlag(); }

inline qreal luminance(const QColor& color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
}

// 越亮的墨翻得越深，保留原有层次差；透明度不变，边缘覆盖仍由 alpha 承担。
inline QColor chrome(const QColor& color)
{
    if (darkFlag() || color.alpha() == 0)
        return color;
    const qreal brightness = luminance(color);
    if (brightness < 0.55)
        return color;
    const qreal depth = std::clamp((brightness - 0.55) / 0.45, 0.0, 1.0);
    const int value = qRound(72.0 - 46.0 * depth);
    return QColor(value, value + 2, value + 7, color.alpha());
}

// 彩色内容保留色相，在亮色表面上略微加深，避免淡彩色发白。
inline QColor accent(const QColor& color)
{
    if (darkFlag() || color.alpha() == 0)
        return color;
    const qreal hue = color.hsvHueF();
    if (hue < 0.0 || color.saturationF() < 0.12)
        return chrome(color);
    return QColor::fromHsvF(hue, std::min<qreal>(1.0, color.saturationF() * 1.06),
                            std::clamp<qreal>(color.valueF() * 0.92, 0.0, 1.0),
                            color.alphaF());
}
}
