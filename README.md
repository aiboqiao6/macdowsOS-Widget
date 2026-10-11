
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

## 自定义小组件

从托盘菜单 **设置… → 自定义小组件 → 打开小组件工作室…** 进入编辑页面，可创建、导入和导出组件。已保存的组件会单独出现在“添加小组件”窗口的 **自定义小组件** 分区，支持搜索和拖放添加；没有自定义组件时隐藏此分区。桌面上的自定义组件可通过顶部右键或双击直接编辑。

- 手动创建、保存、复制、删除组件，支持多实例添加到桌面。
- 低代码模式：文本、大数字、时钟、日期、进度条、分隔线；支持排序、复制、配色、字号、对齐、加粗和边距。
- HTML 模式：HTML / CSS / JavaScript 分区编辑，支持导入完整 HTML 文件和实时预览。
- 三档尺寸、四个起始模板、JSON 导入导出、未保存修改提示、桌面实例同步和重启恢复。
- 编辑与预览两栏布局，按内容块显示适用属性；支持拖动排序、撤销重做。导入成功即加入组件库，预览在重命名或调整窗口大小时保留交互状态。
- 使用系统 Microsoft Edge WebView2 Runtime。构建时自动复制随源码提供的 x64 WebView2Loader.dll；首次运行不自动安装依赖。

操作说明、联网行为和文件格式见 [自定义组件说明](docs/custom-widgets.md)。


# 鸣谢

设计基础来自 [QtGlassFlow](https://github.com/SuperSiyer/QtGlassFlow)。


