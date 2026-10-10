#pragma once
#include <QtGlobal>

// 卡片参考尺寸与布局比例；逻辑尺寸只乘组件缩放，DPR 交由渲染层处理。
namespace CardMetrics {
inline constexpr int kGridBaseCell = 328;
inline constexpr int kBatteryListRows = 6;
// 设备列表包含本机，六个显示项最多容纳五个外设。
inline constexpr int kMaxConnectedDevices = kBatteryListRows - 1;
inline constexpr int kWeatherHourlySlots = 6;
inline constexpr int kWeatherDailySlots = 5;
// 天气分区按 2×2 卡片归一化，布局不受组件缩放与 DPI 影响。
inline constexpr qreal kWeatherSummaryDividerRatio = .278;
inline constexpr qreal kWeatherDailyDividerRatio = .539;
inline constexpr qreal kWeatherHourlyTopRatio = .298;
inline constexpr qreal kWeatherHourlyIconOffsetRatio = .083;
inline constexpr qreal kWeatherHourlyTemperatureOffsetRatio = .170;
inline constexpr qreal kWeatherLargeLocationFontRatio = .070;
inline constexpr qreal kWeatherLargeTemperatureFontRatio = .180;
inline constexpr qreal kWeatherCompactConditionFontRatio = .058;
inline constexpr qreal kWeatherWideConditionFontRatio = .052;
inline constexpr qreal kWeatherLargeConditionFontRatio = .050;
inline constexpr qreal kWeatherConditionGapRatio = .014;
inline constexpr qreal kWeatherLargeRangeFontRatio = .100;
inline constexpr qreal kWeatherLargeHourlyFontRatio = .053;
inline constexpr qreal kWeatherLargeDailyLabelFontRatio = .056;
inline constexpr qreal kWeatherLargeDailyTemperatureFontRatio = .055;
// 时钟几何按可用区域比例计算，保持不同尺寸下的边距一致。
inline constexpr qreal kClockCardInsetRatio = .018;
inline constexpr qreal kClockFaceInsetRatio = .022;
inline constexpr qreal kClockDialInsetRatio = .040;
inline constexpr qreal kClockDialRadiusRatio = .455;
inline constexpr qreal kClockCityLabelHeightRatio = .130;
inline constexpr qreal kClockCityLabelGapRatio = .012;
// 网格间距随组件缩放和屏幕短边适配，保持一致的视觉节奏。
inline constexpr qreal kGridBaseGap = 16.0;

}
