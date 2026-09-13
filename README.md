# macdowsOS Widget

> 面向 Windows 桌面的 Qt/C++ 液态玻璃小组件系统。<br>
> 将电量、天气、时间与词典信息安静地置于桌面底层，以克制、通透、低占用的设计语言呈现信息。

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0A84FF?style=flat-square)
![Qt](https://img.shields.io/badge/Qt-6.4%2B-41CD52?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-5C6BC0?style=flat-square)
![Renderer](https://img.shields.io/badge/Renderer-OpenGL-8A7CFF?style=flat-square)
![License](https://img.shields.io/badge/License-GPL--3.0-EF5350?style=flat-square)

## 设计理念

macdowsOS Widget 不是传统的悬浮窗集合，而是一套可复用的桌面玻璃组件系统：

- 每个组件都是独立窗口，可以单独拖动、吸附、隐藏和删除。
- 所有组件共享同一套液态玻璃底层，圆角、裁切、模糊、折射、边缘高光和透明度统一维护。
- 组件只负责绘制业务内容，不重复实现背景材质和窗口行为。
- 默认固定在桌面底层，不抢占焦点，也不会把前景窗口绘制进玻璃背景。

## 视觉与交互

- 高度透明的液态玻璃表面：局部背景模糊、轻微折射、柔和边缘反光与像素级圆角抗锯齿。
- 只采样组件所在位置的桌面内容，避免整屏截图和前景窗口串入纹理。
- 组件库从屏幕底部中央滑入/滑出，动画期间实时 60 FPS 渲染。
- 从组件库拖动预览卡片到桌面即可添加实例；松手自动吸附到最近的可用网格位置。
- 网格四边保留与格子间距相同的自适应外边距，组件之间不允许重叠。
- 位置、类型、可见状态和实例 ID 会持久化保存，开启鼠标穿透不会重置布局。

## 内置组件

| 组件 | 数据来源与行为 |
| --- | --- |
| 电量 | Windows `GetSystemPowerStatus` 读取真实电量、交流电连接和充电状态；对支持公开电量的蓝牙设备同步显示。 |
| 天气 | 通过 [wttr.in](https://wttr.in/) 获取真实温度、天气描述、最高温和最低温；右键或双击城市标题搜索城市。 |
| 时间 | 读取系统当前时间绘制时钟；静止时约 15 FPS，交互和动画期间自动恢复 60 FPS。 |
| 词典 | 从内置真实英文词集合随机选词，并通过 [Free Dictionary API](https://dictionaryapi.dev/) 获取音标、词性和释义。 |

网络不可用时，天气和词典显示明确的不可用状态，不伪造数据。

## 技术架构

```text
Battery / Weather / Clock / Dictionary
                    │ 只绘制业务内容
                    ▼
          LiquidGlassWidget（通用窗口层）
          ├─ 桌面置底、透明窗口、圆角 mask
          ├─ 桌面背景采样与局部裁切
          ├─ 拖动、鼠标穿透、网格吸附
          └─ 设置同步、动画刷新策略
                    ▼
          QtGlassFlowScene（渲染底层）
          ├─ OpenGL 2.1 / GLSL 120
          ├─ 缩小分辨率 FBO + 双向高斯模糊
          ├─ SDF 圆角、折射采样、抗锯齿
          └─ 液态玻璃对象连接与合成
```

新增组件只需继承 `LiquidGlassWidget` 并实现 `paintOverlay(QPainter&)`，即可复用全部玻璃、裁切、圆角和窗口逻辑。

## 性能设计

- 背景截图与模糊 FBO 默认使用原生像素的 70% 内部渲染分辨率，再由原生窗口帧缓冲双线性合成。
- 共享壁纸画布，尺寸不变时复用 OpenGL 纹理，通过 `glTexSubImage2D` 更新内容。
- 静态组件停止场景计时器，仅由数据或交互事件触发重绘；时钟静止时约 15 FPS。
- 组件库静止时跳过未变化截图的模糊重建；鼠标穿透可开启低功耗刷新（活动刷新约 4 FPS）。
- 添加小组件页在动画期间复用固定覆盖区截图：背景捕获、几何裁切与玻璃合成均保持 60 FPS，并在上传前按实际截图像素缩小，避免重复的大图转换。
- 电量、外设状态无变化时不重复生成托盘图标和窗口内容。
- 支持自动、GPU/OpenGL Shader 与 CPU 软件缓存三种渲染策略，默认使用自动策略。

## 构建

环境要求：Windows 10/11、Visual Studio 2022（桌面 C++ 开发工作负载）、Qt VS Tools、Qt 6.4+ MSVC x64、C++17。
工程当前选择的 Qt 配置名称为 `6.11.1_msvc2022_64`；可在 VS 的 Qt Project Settings 中切换到本机安装的 MSVC x64 版本。

1. 打开仓库根目录的 `macdowsOS Widget.sln`。
2. 选择 `Release | x64`，生成解决方案。
3. 运行 `bin/x64/Release/macdowsOSWidget.exe`。

也可在 VS 2022 Developer PowerShell 中执行：

```powershell
msbuild "macdowsOS Widget.sln" /m /p:Configuration=Release /p:Platform=x64
```

编译中间文件统一写入 `obj/x64/<配置>/`，程序及 Qt 运行库写入 `bin/x64/<配置>/`。Post-Build 调用 `windeployqt` 部署运行库。
项目仅维护 Visual Studio 构建入口。

## 使用与设置

启动后默认恢复四个独立组件。通过托盘图标可以显示/隐藏组件、打开组件库、打开设置或退出程序。组件右键菜单支持删除；天气卡片支持搜索城市。

设置页使用 Qt `QDialog` 实现，不调用 Win32 消息框，提供：

- 显示大小：40%、55%、70%、85%、100%、115%、135%
- 背景材质：液态玻璃 / 仅模糊（兼容 Windows 10 和 Windows 11）
- 液态玻璃模糊程度与玻璃透明度（分别调节）
- 渲染方式：自动 / GPU OpenGL Shader / CPU 软件缓存模糊
- 鼠标穿透、穿透时低功耗刷新、天气城市、Windows 开机自启动

组件布局和实例信息保存于：

```text
%LOCALAPPDATA%\\macdowsOS\\macdowsOS Widget\\widgets.json
```

删除该文件可恢复默认布局；程序不会删除用户壁纸或其他系统数据。

## 开源许可与第三方声明

QtGlassFlow 源码及修改后的渲染代码直接编译进 `macdowsOSWidget.exe`。本项目及其分发版本按 **GNU General Public License v3.0（GPL-3.0）** 提供，并保留上游版权、许可证与免责声明。

上游项目：[SuperSiyer/QtGlassFlow](https://github.com/SuperSiyer/QtGlassFlow) ；源码位于 `third_party/QtGlassFlow/`，完整许可证文本位于该目录的 `LICENSE` 文件。

分发包含 QtGlassFlow 代码的版本时，请：

1. 保留原作者版权、许可证和免责声明。
2. 向接收者提供对应源代码，或提供明确、可执行的源代码获取方式。
3. 标明对 QtGlassFlow 所做的修改，并继续以 GPL-3.0 提供相应覆盖部分。
4. 不得以额外限制削弱 GPL-3.0 授予的权利，也不得将组合程序作为闭源专有软件重新分发。

根目录的 [`LICENSE.txt`](LICENSE.txt) 是项目许可入口；依赖来源、基准版本和修改记录见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。发布包应同时附带上述文件、`third_party/QtGlassFlow/LICENSE`、本 README 和完整对应源代码。Qt 6 按所选发行版的 LGPL/GPL 或商业许可条款执行。

Apple、macOS 及其相关名称和标识是 Apple Inc. 的商标。本项目是独立的开源 Windows 桌面项目，与 Apple Inc. 不存在隶属、授权、认可或合作关系；“macOS 液态玻璃”仅用于描述视觉设计灵感。

## 目录结构

```text
macdowsOS Widget/             # 仓库根目录
├─ macdowsOS Widget.sln       # Visual Studio 入口
├─ macdowsOS Widget.vcxproj
├─ macdowsOS Widget.vcxproj.filters
├─ src/
│  ├─ app/                   # 程序入口
│  ├─ widgets/               # 小组件、数据与布局管理
│  ├─ ui/                    # 组件库界面
│  └─ rendering/             # 通用玻璃窗口层
├─ third_party/QtGlassFlow/
│  ├─ LICENSE
│  ├─ README.md              # 来源、版本及集成说明
│  └─ src/                   # 渲染源码、qrc 与 GLSL
├─ README.md
├─ LICENSE.txt
├─ THIRD_PARTY_NOTICES.md
├─ bin/                      # 生成的程序和运行库（忽略）
└─ obj/                      # 编译中间文件（忽略）
```

第三方源码直接编译进应用，由主仓库统一管理；上游示例、Wiki、Linux 打包文件和独立构建配置不参与项目。
`batterywidget.cpp/.h` 目前还包含天气、时钟、词典和组件管理逻辑，本次按现有职责归档，后续可继续拆分。

## 已知限制

- 外设电量是否可见取决于 Windows 驱动是否公开 `System.Devices.BatteryLife`。
- 天气和词典需要网络；离线时组件保留，但显示不可用状态。
- 玻璃背景默认采样壁纸；添加小组件页面会额外采样其后方已合成桌面内容。

---

**macdowsOS Widget**  ·  把信息、秩序与材质感留在桌面边缘。
