# Third-party notices

macdowsOS Widget 尊重并保留所使用开源项目的版权、许可和免责声明。本文件记录直接进入源码或运行时分发物的第三方组件。

## QtGlassFlow

- 项目：QtGlassFlow
- 上游仓库：https://github.com/SuperSiyer/QtGlassFlow
- 集成基准提交：`c87f4ac980df1a01b4c885b876ad8eacad717c28`
- 许可证：GNU General Public License v3.0（GPL-3.0-only）
- 上游源码：`third_party/QtGlassFlow/`
- 完整许可文本：`third_party/QtGlassFlow/LICENSE`

### 修改声明

本发行版修改了 QtGlassFlow。相关修改于 2026-09-13 汇总标记，主要包括：

- 将渲染场景接入可复用的 `LiquidGlassWidget` 桌面组件底层。
- 增加外部背景图像输入、局部桌面采样与窗口位置对齐。
- 增加玻璃透明度、背景模糊程度和渲染分辨率控制。
- 优化 FBO、纹理复用、模糊流程和按需刷新调度。
- 调整 SDF 圆角、边缘高光、折射与原生像素尺度计算。
- 将内嵌源码统一移至根目录 third_party，仅由 Visual Studio 主工程构建；移出上游示例、Wiki、图片和独立打包/构建配置。

修改文件顶部保留了醒目的修改日期和许可指引。QtGlassFlow 的上游许可文本没有被删除或替换。

## Qt 6

本项目使用 Qt 6 Core、Gui、Widgets、Network、OpenGL 和 OpenGLWidgets。Qt 的使用与分发必须遵循实际采用版本对应的 LGPL、GPL 或商业许可条款。部署应用时应按所选许可提供所需通知、许可证文本和可替换/可重新链接条件。

- 官方许可说明：https://www.qt.io/licensing/
- Qt 开源许可说明：https://www.qt.io/download-open-source/

## 网络服务

以下服务不会作为源码库链接进应用，但程序运行时会访问其公开接口：

- wttr.in：https://wttr.in/
- Free Dictionary API：https://dictionaryapi.dev/

服务的可用性、速率限制、数据质量和使用条款由各自提供方负责。

## 再分发提醒

发布源码、便携包或安装程序前，请确认：

1. 包含根目录 `LICENSE.txt`、本文件和 QtGlassFlow 的完整 `LICENSE`。
2. 向接收者提供与二进制精确对应的完整源码及构建所需脚本。
3. 保留上游声明与本项目对修改文件所作的醒目标记。
4. 不附加与 GPL-3.0 冲突的最终用户许可或技术限制。

本文件用于记录项目依赖和发布注意事项，不构成法律意见。
