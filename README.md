
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

# 项目结构

- `src/app`：应用入口与启动逻辑
- `src/widgets`：电池、天气、时钟、词典组件
- `src/ui`：小组件选择窗口
- `src/rendering`：液态玻璃、桌面采集和响应式布局
- `third_party/QtGlassFlow`：液态玻璃渲染依赖
- `weather`：天气图标与运行时资源
- `tests`：回归测试
- `x64/Release`：Visual Studio Release 可执行文件及运行库

项目只使用 Visual Studio 工程构建；Release 输出写入 `x64/Release`，构建中间文件和测试截图不会纳入源码管理。

# 鸣谢

液态玻璃设计基于[QtGlassFlow](https://github.com/SuperSiyer/QtGlassFlow)

