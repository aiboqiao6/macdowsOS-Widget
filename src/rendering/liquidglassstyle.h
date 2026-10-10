#pragma once

#include <algorithm>

// 所有玻璃表面共用固定材质；画质档位只控制采样密度，不切换光学样式。
namespace LiquidGlassStyle {
inline constexpr float kShapePower = 3.8f;
inline constexpr float kRefractionPower = 1.32f;
inline constexpr float kBlurRadius = 1.15f;
inline constexpr int kBlurIterations = 2;
inline constexpr float kNoiseAmount = 0.008f;
inline constexpr float kCardCornerRadius = 52.0f;

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
