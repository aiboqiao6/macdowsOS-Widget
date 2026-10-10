
<div align="center">

# macdowsOS Widget

仿macOS样式的桌面小组件

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0A84FF?style=flat-square)
![Qt](https://img.shields.io/badge/Qt-6.4%2B-41CD52?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-5C6BC0?style=flat-square)
![Renderer](https://img.shields.io/badge/Renderer-OpenGL-8A7CFF?style=flat-square)
![License](https://img.shields.io/badge/License-GPL--3.0-EF5350?style=flat-square)

</div>

# 简介

基于Qt的仿macOS样式的第三方桌面小组件


# 鸣谢

液态玻璃渲染已融合到项目的 `src/rendering`，设计基础来自 [QtGlassFlow](https://github.com/SuperSiyer/QtGlassFlow)。

# 渲染诊断日志

程序启动后自动写入 UTF-8 日志：

`%LOCALAPPDATA%\macdowsOS\macdowsOS Widget\logs\render.log`

托盘图标右键选择“打开渲染日志文件夹”即可打开所在目录。每个文件最多 2 MB，保留 `render.log.1`、`render.log.2`、`render.log.3` 三份历史文件，总计不超过 8 MB。时间戳使用 UTC。

日志记录应用版本、Qt/Windows 版本、显示器缩放与刷新率、OpenGL 显卡/驱动字符串、透明通道、着色器与 FBO 创建、每 5 秒的帧率汇总，以及纹理上传、绘制、截图回读的 GL 错误。Windows 图形捕获失败保留 HRESULT；窗口层级调整失败保留 Windows 错误码和目标窗口 PID。重复错误限频并统计被省略的次数。日志不保存截图或组件内容。

排查 MyDockFinder 托盘列表花屏时，在问题出现后保留当前日志和三份历史日志，并注明出现花屏的本地时间、托盘关闭后是否恢复。通过 `window.foreground` / `window.topology`、`gl.error`、`capture.failure`、`window.layer.failed` 和 `render.heartbeat` 对照发生前后的渲染与窗口状态。

