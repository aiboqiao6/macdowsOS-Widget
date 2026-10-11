# 自定义组件验证记录

环境：Windows 11、Qt 6.11.1 / MSVC x64、Microsoft Edge WebView2 Runtime 154.0.4258.62。

## 已通过

- Visual Studio Release x64 构建，自动部署 WebView2Loader.dll。官方 Loader 的 Authenticode 签名验证有效，签署者为 Microsoft Corporation。
- 六项定向回归全部通过（包含初始化、清理共 8 passed）：导入导出及输入校验、损坏组件库保护、桌面编辑同步及快照恢复、保存失败时的布局回滚、编辑页面保存、真实 HTML/CSS/JavaScript 和 iframe 隔离。
- 追加原有菜单、隐藏布局恢复与音乐配置三项验证后，共 11 passed、0 failed。编辑页面还验证了内容块添加、上移、复制、删除和新建草稿的未保存状态。
- HTML 回归通过真实脚本点击按钮，检查计算后的 CSS 背景色、跨框架宿主读取被阻止，以及默认关闭网络时触发浏览器 connect-src 拦截。
- 实际窗口检查：HTML 计数器按钮点击后从 0 更新为 1；“保存并添加到桌面”成功创建实例，组件库同步出现保存项目。
- 预览自动等比例适应面板，并显示桌面实际尺寸。
- 设置入口和独立图库分区追加验证完成：20 项功能用例全部通过，包含初始化、清理共 22 passed、0 failed。覆盖设置内工作室可操作、返回后保留未提交设置、空库隐藏分类、搜索与分区、保存/导入/删除刷新、拖动取消与延迟刷新，以及正确组件和尺寸的桌面放置。原有图库筛选、缩放、菜单、配置恢复和材质保存用例也通过。
- 已检查设置入口与自定义图库实际窗口截图，布局和名称、尺寸标签显示正常。截图位于 `obj/regression/artifacts/settings-custom-widget-entry.png`、`obj/regression/artifacts/gallery-custom-widgets.png`，最新报告为 `obj/regression/custom-gallery-final.txt`。
- 中文注释审查及 git diff --check 通过。

## 工作室可用性复查

旧的设置入口回归使用程序调用 `click()`，仅能验证 Qt 状态与保存流程，不能证明 Windows 原生窗口接收真实鼠标输入。新增检查要求工作室以设置窗口为父窗口并使用窗口模态，同时检查 Win32 `IsWindowEnabled` 和 `GW_OWNER`，避免 Qt 控件显示可用而原生窗口仍被禁用。

设置窗口改为独立顶层窗口，不再继承桌面卡片的非激活窗口关系；工作室仍由设置拥有，关闭后返回设置。工作室改为编辑和预览两栏，底部固定保存入口，按内容块类型显示属性，颜色与边距默认折叠。增加全局撤销/重做、内容块拖动排序、导入后立即入库、预览重载，以及重命名和调整窗口大小时保留交互状态。

最终 Release x64 和回归程序构建成功。本轮相关回归结果为 **30 passed、0 failed、0 skipped**（含初始化、清理与设置的两组数据），报告为 `obj/regression/editor-usability-final.txt`。覆盖关闭尚未加载完成的预览、重载成功后的状态恢复、编辑和导入导出、自定义图库及原有菜单、缩放和配置行为。

实际鼠标检查从设置入口开始，完成新建 HTML 模板、聚焦名称输入框、全选并输入中文名称、点击保存；组件列表更新为新名称，底部显示保存成功。原生日志确认工作室启用且由设置窗口拥有，报告为 `obj/regression/editor-settings-mouse.txt`。设置内嵌 WebView2 的点击因工具将其识别为其他进程而未能验证；另开独立工作室窗口后，真实点击 HTML 计数器成功从 0 变为 1。该检查使用 `computer-use` 的窗口截图及输入接口，报告为 `obj/regression/editor-preview-mouse.txt`。

回归过程中发现并修复预览尚未启动完成时关闭窗口的崩溃：派生析构前使独立存活标记失效，迟到的引擎回调不再访问视图；COM 初始化保留到 GUI 线程退出。修复后同一组用例全部通过。Qt 的 `grab()` 截图不能捕获 WebView2 原生子窗口，其预览区域可能为空；HTML 交互结果以实际 Windows 窗口检查和脚本回归为准。

`customEditorManualVisualCheck` 支持从设置开始的真实鼠标检查。此模式不会自动点击工作室入口，使用独立测试配置，不修改正式组件库；超时会清理未保存状态并关闭编辑器和设置，最长运行 180 秒。在 `obj/regression` 运行：

```powershell
$env:PATH = 'C:/Qt/6.11.1/msvc2022_64/bin;' + $env:PATH
$env:WIDGET_EDITOR_OPEN_SETTINGS = '1'
$env:WIDGET_EDITOR_PREVIEW_MS = '120000'
./release/widget_regression.exe customEditorManualVisualCheck -o custom-editor-settings-manual.txt,txt
```

如果自动化工具无法列出设置工具窗，可将 `WIDGET_EDITOR_OPEN_SETTINGS` 设为 `2`。该模式只在 500 ms 后程序调用设置入口，工作室显示后的按钮与预览仍须真实鼠标检查。此模式不能代替对设置入口本身的真实点击验证。

检查时使用真实鼠标打开入口、切换模式、编辑文字并操作预览。截图输出到 `artifacts/custom-editor-settings-start.png` 和 `artifacts/custom-editor-from-settings.png`。自动回归还检查 Enter 不误触按钮；按内容块类型显示相关属性；撤销/重做文字与块的增删并正确清除旧分支；720×540 窗口中的保存入口；重命名、缩放窗口时保留 HTML 交互状态，修改 HTML 源码时仍重新加载，并验证重新创建预览后正常加载。设置入口同时覆盖新建工作室与复用桌面打开的非模态工作室。导入回归验证 HTML 与 JSON 导入后无需二次保存即可进入组件库和图库，失败的导入保留已有内容。

## 完整回归的限制

一次完整运行结果为 117 passed、14 failed、4 skipped。失败集中在现有实时背景捕获及其帧率、颜色、更新测试。另开独立测试进程，仅运行 `liveBackdropAndStableCapture`、`firstRunWizardFollowsMovingBackground`，同样失败；该进程没有创建自定义组件或 WebView2 视图。

本次没有修改实时背景捕获实现，未确定这些失败的根因。记录保留在 `obj/regression/full-custom-widgets.txt` 与 `obj/regression/capture-isolation.txt`。后续六项定向回归的最新结果在 `obj/regression/custom-widgets-final.txt`。

完整运行中的四项跳过为需显式环境参数的编辑窗口检查、管线基准、主题参考图，以及仅供辅助进程使用的动态背景源。
