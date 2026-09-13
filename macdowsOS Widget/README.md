# macdowsOS Widget

一个使用 Qt 6 Widgets/C++ 编写的桌面小组件原型，目标是还原 macOS 液态玻璃（Liquid Glass）视觉语言。应用显示名为 `macdowsOS Widget`。

## 通用液态玻璃底层

`LiquidGlassWidget` 是所有小组件共用的底层：它封装了 `QtGlassFlowScene` 的 OpenGL FBO、分离式高斯模糊、折射采样、SDF 圆角/抗锯齿、透明窗口、圆角 mask、桌面背景采样、右下角定位和拖动。背景截图与模糊 FBO 默认使用 70% 内部渲染分辨率，再由原生窗口帧缓冲双线性放大，因此组件最终尺寸和圆角不变，同时显著降低截图上传、纹理带宽与模糊填充开销。无动画组件在静止时停止场景计时器并仅按需重绘，时钟保留约 15 FPS 的平滑秒针；桌面背景检测在静止时降为 1 Hz，拖动时自动恢复 60 Hz。新组件只需继承它并实现 `paintOverlay(QPainter&)`，不需要重复处理玻璃渲染。

底层实现来自并内置于项目的 [QtGlassFlow](https://github.com/SuperSiyer/QtGlassFlow)（按其仓库 GPL-3.0 许可证保留），着色器资源通过 `shaders.qrc` 编译进程序。

```cpp
class CpuWidget final : public LiquidGlassWidget {
protected:
    void paintOverlay(QPainter& painter) override; // 只画业务内容
};
```

## 当前能力

- 通过 Windows `GetSystemPowerStatus` 读取真实电量、AC 连接和充电状态，每 2 秒刷新。
- 通过 Windows Bluetooth API 同步当前已连接外设，并从 Function Discovery 读取驱动公开的 `System.Devices.BatteryLife`；无电量信息时只显示“已连接”，不使用模拟百分比。
- 无边框、不抢焦点且通过 `HWND_BOTTOM` 固定在桌面底层，默认贴靠当前屏幕右下角；拖动时以 60 FPS 响应，静止的无动画卡片采用事件驱动（不持续渲染），时钟约 15 FPS；开启“鼠标穿透时降低刷新率”后活动刷新降至 4 FPS。底图只使用 Windows 当前壁纸中与组件位置对应的裁切区域，因此前景窗口和桌面文字不会进入玻璃纹理，也不绘制额外背景遮罩。
- 电池图标完全由 Qt `QPainter` 绘制，不依赖外部图标版权资源；低电量、充电状态使用不同色彩。
- 默认显示四个独立桌面小组件：电量（四个设备环）、天气、模拟时钟和词典卡，布局与参考图对应；每个窗口可单独拖动，托盘菜单可一键重新排列四个组件，支持高 DPI 屏幕。桌面网格四边保留与格子之间相同的自适应间距，候选位置、吸附、保存和恢复使用同一网格原点。
- 托盘中的“添加小组件…”会打开可拖拽组件库；拖动卡片到桌面即可创建新的独立窗口。窗口位置按当前显示缩放倍率吸附到桌面网格，组件类型、实例 ID、位置和可见状态保存到 `%LOCALAPPDATA%/macdowsOS/macdowsOS Widget/widgets.json`，下次启动自动恢复。
- 组件库打开和关闭时仅在 290ms 几何动画期间启用 60 FPS 场景与背景采样，确保液态玻璃背景随窗口每一帧移动；动画结束后场景计时器停止，背景检测降为 1 Hz，并跳过未变化截图的模糊重建。
- 组件库左侧暂时只保留“所有小组件”文字分类，不显示分类图标或无效入口；窗口空白区域不可拖动，只有预览卡片会启动添加组件的拖拽，预览卡片使用与桌面组件一致的玻璃材质和内容比例。
- 天气通过 Qt Network 请求 `wttr.in`，默认城市为 Beijing；设置页可修改城市，数据每 10 分钟刷新，离线时显示“天气暂不可用”。
- 天气卡右键“搜索城市…”可直接输入城市或地区，天气字段来自 `wttr.in` 实时响应；词典卡每 30 分钟从真实英文词集合中随机选择一个单词，并从 `dictionaryapi.dev` 获取音标、词性和释义，网络不可用时显示状态提示。
- 系统托盘图标支持右键菜单：显示/隐藏组件、关于、退出，以及无需管理员权限的 Windows 开机自启动开关。
- 设置页使用纯 Qt `QDialog`，可选择 40% / 55% / 70% / 85% / 100% / 115% / 135% 显示比例、天气城市、0%–100% 模糊程度、5%–100% 透明度，切换 Qt 液态玻璃或 Windows 10/11 兼容的仅模糊材质，并可启用鼠标穿透；鼠标穿透时可选 4 FPS 低刷新率以节省性能；关于页同样不调用 Win32 或系统消息框。

## 构建

### Qt Creator / CMake

打开本目录的 `CMakeLists.txt`，选择 Qt 6.4 或更高版本（Windows MSVC x64）。CMake 的 Post-Build 会自动调用 `windeployqt`，将 `Qt6Gui.dll`、`Qt6Widgets.dll`、`platforms/qwindows.dll` 等运行库复制到 exe 同目录。

### Visual Studio Qt VS Tools

项目已补齐 `batterywidget.cpp/.h`、`liquidglasswidget.cpp/.h` 与 `main.cpp`，并将 Qt 模块设置为 `core;gui;widgets;network;opengl;openglwidgets`。在 Qt VS Tools 中选择 `6.11.1_msvc2022_64` 后直接生成即可。

启动后四个组件会以参考图布局出现在当前屏幕右下角。桌面机没有电池时，电量组件会显示适配器状态。
