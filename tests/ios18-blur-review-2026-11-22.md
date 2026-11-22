# iOS 18 模糊样式验证

## 修改

- `rendering/liquidglassstyle.h` 增加材质样式枚举（0 液态玻璃、1 iOS 18 模糊）与 `StyleOptics` 预设。iOS 18 预设为折射 0、模糊半径 12.0、六次迭代、颗粒 0.008、饱和度 1.22–1.36、中性色洗 0.44/0.50（面板）与 0.82（卡片亮色）、描边 0.20–0.30。编号写入设置文件，只能追加。模糊上限同步提高到半径 32、次数 8，模糊核步长随半径自适应，保证 64 项采样表不溢出。
- `shaders/scene_fragment.glsl` 增加样式分支：iOS 18 材质只做磨砂、按亮度放大色差、叠一层中性色洗，并在轮廓内侧描一条 1 设备像素亮线；不参与曲面折射、边缘反光和内沿暗边。液态玻璃分支逐条保留，未改动其运算顺序。
- `rendering/qtglassflowscene.*` 增加 `setMaterialStyle` 与 `styleParameters`。样式从 `material/style` 读取，构造函数以样式预设作为材质默认值，用户保存的参数仍优先；亮暗模式切换只更新洗色与描边强度，不覆盖可调参数。
- 材质设置页新增“材质样式”下拉框：选中后把预设填回五个控件并立即应用到全部玻璃表面，同时写入 `material/style`；iOS 18 下“折射强度”禁用；“恢复默认材质”还原成当前样式的预设而不是固定还原成液态玻璃。
- 组件文字始终是浅色，所以 iOS 18 的色洗朝中性深色收，避免浅色面板吞掉文字，这是与真机浅色小组件唯一有意的偏离。该偏离已在 2026-11-22 的卡片亮色模式中解除：卡片在亮色模式下使用白色洗与深色墨，见 `tests/light-mode-review-2026-11-22.md`。

## 验证

- Release x64 应用构建成功，回归程序构建成功，未发现编译错误。
- 运行时着色器编译日志为 `gl.shaders ready=1 blit=1 blur=1 glass=1`，两种样式均通过，说明新增 uniform 与分支没有破坏 GLSL 1.20 编链。
- 真实桌面截图对比（DPR 1.5、六张卡片）：液态玻璃呈现青绿色透镜与明亮外沿反光；iOS 18 模糊呈现平整的中性磨砂，壁纸只剩明暗与色彩，只有一条细亮边，文字对比度不变。截图为 `artifacts/ios18-blur/desktop-liquid-glass.png` 与 `artifacts/ios18-blur/desktop-ios18-blur.png`。
- 针对性回归：`ios18BlurStyleUsesFlatFrostMaterial` 覆盖样式预设、亮暗切换与切回液态玻璃；`materialStylePickerAppliesPresetToEverySurface` 覆盖下拉框应用、折射项禁用与重启后沿用；`materialSettingsApplyImmediatelyAndSurviveEscape`、`materialSettingsPersistAndResetToOriginalDefaults`、`unifiedGlassUsesSingleOpticalMaterialWithoutOverlayMasks` 继续通过，确认液态玻璃默认外观未变。
- 完整回归在本机没有跑完：这台机器上 OBS、Krita 与浏览器共用同一块显卡，整套用例压满显存后被中断。中断前失败的项集中在设置页几何断言（`settingsScaleFollowsSliderAndFitsScreen`、`settingsBottomRightCornerStaysRounded`），与本次改动无关的可能性较大——本次只新增一行下拉选择项和一个禁用的折射输入——但仍需在性能余量充足的环境中复核，不要据此认为整套用例已通过。
