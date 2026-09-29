# 2026-09-23 组件库与右键菜单文字清晰度

全局继续使用现有的 PingFang：桌面卡片、组件库、搜索框、菜单和设置控件均未切换字体。

## 原因与修复

- 原最高档将文字转换为无 hinting 的四倍轮廓图，再缩小。此前的小字号保护只覆盖最终字号小于 22 像素的情况；16 逻辑像素的菜单在 150% DPI 下仍进入这条路径。现将清晰/最高档的自绘文字按最终物理字号交给 Qt 栅格器，保留字体的 hinting 设置，基线对齐物理像素，覆盖图按 1:1 合成。透明背景使用灰阶覆盖；缓存仍限制为 8 MiB，背景画质独立于文字分辨率。
- 组件预览原先固定绘制为两倍图片，再根据 DPI 缩放到屏幕。现在预览接收目标 DPR，使用逻辑布局和原生大小的图像；显示时直接按物理像素对齐，不再二次过滤。缓存区分 DPR 与抗锯齿档位，显示和 DPI 改变时刷新。
- 侧栏图标原先来自固定 30×30 位图，高 DPI 下需要放大。现在通过 `QIconEngine` 在请求的分辨率直接绘制。
- 最高档设置名称改为“最高（原生像素）”，数值仍为 3，已有配置继续生效。

Qt 的相关行为说明：[字体与 hinting](https://doc.qt.io/qt-6/qfont.html#HintingPreference-enum)、[高 DPI 图像与设备像素比](https://doc.qt.io/qt-6/highdpi.html#drawing)。本文取代旧记录中“最高档使用四倍字形轮廓超采样”的当前实现描述。

## 验证

- `clearTextMatchesNativeRasterAtFractionalScale`：清晰/最高两档，12/16/28 像素文字，70%/100% 界面缩放，以及 100%/125%/150%/200% DPR，共 48 组中文与英文组合逐像素对照 Qt 原生栅格结果。此检查不再把“半透明像素更多”等同于“更清晰”。
- `galleryPreviewsUseNativePixels`：验证四种 DPR 的预览尺寸、实际卡片边缘无再次过滤，以及图标可提供真正的 2× 像素。
- `galleryDictionaryTextAppearance`：保存实际组件库截图，并检查应用及所有组件库控件仍使用 PingFang。
- 四种实际窗口 DPR 的专项测试均为 7 通过、0 失败，日志在 `obj/regression/text-native-dpi-100.txt`、`text-native-dpi-1.25.txt`、`text-native-dpi-1.5.txt`、`text-native-dpi-2.txt`。菜单同时覆盖 CPU/GPU，文字像素不受背景质量影响。
- 第一轮完整回归为 77 通过、2 失败。新增预览测试误用会随布局顺序变化的 QObject 子列表，已经改为使用稳定的卡片列表；另一个普通窗口背景捕获检查单独复测通过，未修改其断言或渲染代码。原日志保留为 `text-native-full.txt`，定向复测 `text-native-retest.txt` 为 6 通过、0 失败。
- 最终完整回归 **79 通过、0 失败**（83.05 秒），见 `obj/regression/text-native-final-full.txt`。涵盖文字、天气、菜单输入、组件库拖放、桌面捕获、CPU/GPU 渲染及生命周期检查。

实际截图位于 `obj/regression/artifacts/`：`gallery-dictionary-native.png`、`glass-menu-screen-1.png`、`glass-menu-screen-2.png`。修复前截图保留在 `artifacts/text-before/`。这些检查覆盖上述窗口和缩放组合，不代表已做跨物理显示器拖动的人工验证。

Release 与 Debug 已构建到 VS 默认的 `x64/Release/`、`x64/Debug/`，未覆盖输出目录。日志为 `obj/text-native-release-build.log` 与 `obj/text-native-debug-build.log`，无编译警告或错误；最终 `git diff --check` 通过。
