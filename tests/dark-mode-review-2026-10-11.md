# 暗色模式实现与验证

## 行为

设置新增“外观模式：亮色 / 暗色”，默认保持原有亮色。接受设置后持久保存 `appearance/darkMode`，同步已有桌面组件、添加小组件窗口、菜单与启动向导；新建表面和下次启动继承该模式。取消设置不改变模式。

暗色沿用现有折射、模糊和动态壁纸采样，在同一着色器中降低透光亮度和边缘反光。切换复用背景纹理与模糊缓存，不新增捕获或渲染通道。添加窗口设计基准及设置缩放规则保持不变。

以用户提供的鹈鹕壁纸参考图校准暗色透光色彩，调整柔白内容、电量圆环粗细、天气图标比例。使用本地同款壁纸对齐后生成离线数据对照图，检查玻璃色调、轮廓与内容布局。字体、图标细节及天气数据与参考仍有差异，不把对照图视为逐像素复刻或实际在线天气截图。

对照产物：

- `obj/regression/artifacts/dark-mode-reference-dark.png`
- `obj/regression/artifacts/dark-mode-reference-light.png`
- `obj/regression/artifacts/dark-mode-reference-wallpaper.png`

## 验证结果

- 最终定向检查：9 项通过、零失败、零跳过（含初始化与清理），日志 `obj/regression/dark-mode-targeted-final.txt`。覆盖暗色切换、缓存复用、透明边角、动态背景更新、全局同步与新窗口继承、设置接受/取消、配置迁移、参考图、亮色管线及电池变体。
- 亮色像素对比：5 档画质 × 3 条渲染路径共 15 张图，全部最大通道差值为 0。基线目录为 `obj/regression/artifacts/pipeline-before-dark-mode`，结果目录为 `obj/regression/artifacts/pipeline-after-dark-mode`。
- 独立测试进程分别设置 `QT_SCREEN_SCALE_FACTORS=1、1.25、1.5、1.75、2`，每组 6 项通过、零失败（含初始化与清理），覆盖模式切换、设置保存、图库尺寸与原生像素预览。日志为 `obj/regression/dark-mode-dpi-1.txt`、`dark-mode-dpi-1.25.txt` 和 `dark-mode-dpi-{1.5,1.75,2}-final.txt`。首次 1.5 组的菜单暴露等待超时，完整重跑该组通过。
- 中文注释检查：65 个源码文件、417 条注释，零问题。`git diff --check` 通过。
- Release 应用构建成功，产物 `artifacts/dark-mode-build/macdowsOSWidget.exe`。

## 完整回归的限制

完整回归在最终暗色圆环和天气图标细节调整之前运行，结果为 115 项通过、2 项失败、2 项按条件跳过，日志 `obj/regression/dark-mode-full.txt`。上述最终定向检查已覆盖后续细节调整；未宣称最终完整套件全部通过。

动态背景采样在完整套件中未达到速度阈值，单独复测通过：1040 毫秒内 36 个源帧、29 次采样、19 次背景变化，日志 `obj/regression/dark-mode-live-layer-isolated.txt`。

`stationaryCardsRejectNativeRaisesAndRecoverTheirLayer` 的窗口层级断言在当前版本和修改前保存的回归程序中均失败，修改前证据为 `obj/regression/dark-mode-layer-before-change.txt`。该既有问题未在本次暗色实现中修复，也没有放宽断言。

## 程序更新

默认启动路径仍为 `x64/Release/macdowsOSWidget.exe`。替换时先保留带时间戳的旧程序备份，再复制 Release 产物并验证 SHA-256；不终止正在运行的用户程序。从托盘退出旧程序后重新打开，在设置中选择暗色即可使用。

已完成替换并校验：备份为 `x64/Release/macdowsOSWidget-before-dark-mode-20261011-060040.exe`，新程序 SHA-256 为 `6DECF0EA1E420D24326A2EC73AD3B71C33648A598E79FFE66F6BA1CA79725AA7`。
