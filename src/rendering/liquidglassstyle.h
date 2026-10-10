#pragma once

// 所有玻璃表面共用固定材质；画质档位只控制采样密度，不切换光学样式。
namespace LiquidGlassStyle {
inline constexpr float kShapePower = 3.8f;
inline constexpr float kRefractionPower = 1.32f;
inline constexpr float kBlurRadius = 4.5f;
inline constexpr int kBlurIterations = 3;
inline constexpr float kNoiseAmount = 0.008f;
}
