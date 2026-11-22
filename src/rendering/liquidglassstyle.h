#pragma once

#include <algorithm>

// 所有玻璃表面共用光学参数；亮暗模式改变透光与反光，画质档位只控制采样密度。
namespace LiquidGlassStyle {
inline constexpr float kShapePower = 3.8f;
inline constexpr float kRefractionPower = 1.32f;
inline constexpr float kBlurRadius = 1.15f;
inline constexpr int kBlurIterations = 2;
inline constexpr float kNoiseAmount = 0.008f;
inline constexpr float kCardCornerRadius = 52.0f;

// 材质样式决定表面的光学模型，画质档位只控制采样密度。
// 液态玻璃靠曲面折射、边缘反光和超椭圆轮廓形成透镜感；
// iOS 18 模糊靠高强度磨砂、整体饱和度提升和 1 像素亮线描边，表面是平的，没有折射。
// 编号会写入设置文件，新增样式只能追加在末尾。
enum class Style { LiquidGlass = 0, Ios18Blur = 1 };
inline constexpr Style kDefaultStyle = Style::LiquidGlass;
inline constexpr int kStyleCount = 2;

// 样式预设。前五项是材质页可调的参数，其余由样式本身决定、不对外暴露：
// frost 是叠在磨砂之上的中性色洗强度，saturation 是背景饱和度倍率，
// hairline 是轮廓内 1 设备像素描边的不透明度，milk 是液态玻璃在亮色模式下抬成白色的强度。
struct StyleOptics {
    float opacity;
    float refraction;
    float blurRadius;
    int blurIterations;
    float noise;
    float frost;
    float saturation;
    float hairline;
    float milk;
};

// 亮色模式把表面抬成半透明白磨砂，卡片内容同时改用深色墨（见 widgets/cardink.h）；
// 暗色模式保留原来的深玻璃与浅色文字。
// lightSurface 只对卡片开放：右键菜单、图库等面板保持深色表面，否则它们的浅色文字会失去对比。
// iOS 18 的磨砂比液态玻璃重得多：更大的高斯半径配合三次迭代，
// 让壁纸只剩下明暗与色彩，这正是它在真机上的观感。
inline constexpr StyleOptics optics(Style style, bool dark, bool lightSurface = false)
{
    const bool white = lightSurface && !dark;
    if (style == Style::Ios18Blur) {
        if (dark)
            return StyleOptics{1.0f, 0.0f, 12.0f, 6, kNoiseAmount, 0.50f, 1.30f, 0.20f, 0.0f};
        return white
            ? StyleOptics{1.0f, 0.0f, 12.0f, 6, kNoiseAmount, 0.82f, 1.22f, 0.26f, 0.0f}
            : StyleOptics{1.0f, 0.0f, 12.0f, 6, kNoiseAmount, 0.44f, 1.36f, 0.30f, 0.0f};
    }
    return StyleOptics{1.0f, kRefractionPower, kBlurRadius, kBlurIterations, kNoiseAmount,
                       0.0f, 1.0f, 0.0f, white ? 0.72f : 0.0f};
}

// 设置文件只保存编号；越界或旧值一律回到默认样式。
inline constexpr Style styleFromIndex(int index)
{
    return index == int(Style::Ios18Blur) ? Style::Ios18Blur : kDefaultStyle;
}

inline constexpr int styleIndex(Style style)
{
    return static_cast<int>(style);
}

// 同一材质按圆角与短边的比例形成曲面；大面板有更宽的平面，避免整面透镜变形。
inline float curvature(float halfShortSide, float cornerRadius)
{
    if (cornerRadius <= 0.0f)
        return 1.0f;
    const float ratio = cornerRadius / std::max(halfShortSide, 1.0f);
    const float t = std::clamp((ratio - 0.07f) / 0.18f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}
