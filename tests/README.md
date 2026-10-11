# 回归测试

在 Windows 的 **x64 Native Tools Command Prompt for VS 2022** 中运行。将 Qt 6 的 MSVC 64 位 `bin` 目录加入 PATH；当前验证环境为 Qt 6.11.1、MSVC v143、Windows 11。

从项目根目录构建：

```bat
set "PATH=C:\Qt\6.11.1\msvc2022_64\bin;%PATH%"
mkdir obj\regression
cd obj\regression
qmake ..\..\tests\widget_regression.pro CONFIG+=release CONFIG-=debug
nmake /nologo
release\widget_regression.exe -o full.txt,txt
```

持续运行测试（默认测试仅运行数秒；下面运行两分钟）：

```bat
set WIDGET_STRESS_MS=120000
release\widget_regression.exe liveCaptureSoak captureSurvivesResizeRecreationAndCancelledReceivers -o stress.txt,txt
set WIDGET_STRESS_MS=
```

`liveCaptureSoak` 持续改变背景颜色和窗口尺寸，检查渲染帧、背景变化，以及预热前后的进程私有内存、句柄数、GDI 对象数。可增加 `WIDGET_STRESS_MS` 进行更长时间验证。

测试使用临时 INI 配置、Qt 测试数据目录和不可达的本地代理，避免修改正式设置或请求在线天气/词典服务。渲染测试需要已登录且未锁屏的交互式 Windows 桌面，会显示测试窗口；不要与其他 UI 自动化并行运行。图像输出保存在运行目录的 `artifacts` 下。

中文注释审查从项目根目录运行：

```bat
python -X utf8 tests\review_comments.py
```

自定义组件定向回归：

```bat
release\widget_regression.exe ^
  customWidgetValidationAndImportExport customCatalogPreservesBrokenFiles ^
  customWidgetDesktopUpdatesAndRestoresSnapshot customSaveRollsBackOnCatalogFailure ^
  customEditorMaintainsBlocksAndSaves customEditorImportImmediatelyAddsToCatalogAndGallery ^
  customEditorButtonsDoNotTreatReturnAsAnAction customEditorShowsOnlyPropertiesForTheSelectedBlock ^
  customEditorUndoRedoRestoresBlockContentAndStructure customEditorBlockDragOrderMatchesSavedOrder ^
  customEditorKeepsPrimaryActionsInsideCompactWindow customEditorKeepsInteractivePreviewWhenRenamedOrResized ^
  customSettingsOpensUsableEditorAndRetainsPendingSettings ^
  customGallerySeparatesSearchesAndRefreshesCatalog customGalleryDefersRefreshUntilDragEnds ^
  customGalleryDropAddsMatchingDefinitionAndSize customHtmlRunsScriptsInsideSandbox ^
  -o custom-widgets.txt,txt
```

`customHtmlRunsScriptsInsideSandbox` 使用系统 WebView2 Runtime 验证真实 CSS、按钮脚本和 iframe 隔离，不访问在线服务。构建后自动复制官方 WebView2Loader.dll。测试组件库与正式配置隔离。

自定义图库用例覆盖空库隐藏、独立分类、搜索、保存与导入后的刷新、删除后移除、拖动取消、拖动中的延迟刷新，以及将正确的组件和尺寸添加到桌面。设置用例同时覆盖新建与复用工作室，检查设置窗口的独立原生 owner、工作室的窗口模态关系及 Win32 输入启用状态，并在返回设置后保留未提交的选择。

本轮工作室用例范围：

- `customEditorImportImmediatelyAddsToCatalogAndGallery` 检查 HTML / JSON 导入后立即进入组件库和图库，重复导入使用新 ID，失败时保留已有内容。
- `customEditorButtonsDoNotTreatReturnAsAnAction` 检查 Enter 不触发新建、保存等默认按钮操作。
- `customEditorShowsOnlyPropertiesForTheSelectedBlock` 检查文本、时钟、进度和分隔线只显示适用属性，外观默认折叠，联网选项仅显示于 HTML 模式。
- `customEditorUndoRedoRestoresBlockContentAndStructure` 检查文字编辑快捷键、内容块添加的撤销重做，以及撤销后新修改会清除旧的重做分支。
- `customEditorBlockDragOrderMatchesSavedOrder` 检查拖动排序同步到保存内容，并可撤销恢复原顺序。
- `customEditorKeepsPrimaryActionsInsideCompactWindow` 检查 720×540 窗口内的名称输入和固定底部保存按钮。
- `customEditorKeepsInteractivePreviewWhenRenamedOrResized` 检查重命名、调整窗口大小时保留 HTML 交互状态，修改源码与重新启动预览时正确加载。

手动检查工作室界面：

```bat
set WIDGET_EDITOR_OPEN_SETTINGS=
set WIDGET_EDITOR_PREVIEW_MS=60000
release\widget_regression.exe customEditorManualVisualCheck -o custom-editor-visual.txt,txt
set WIDGET_EDITOR_PREVIEW_MS=
```

窗口将在指定毫秒数后关闭；输出截图至 `artifacts/custom-widget-editor.png`。此项默认跳过，不改变正式组件库。

从设置开始做真实鼠标检查：

```bat
set WIDGET_EDITOR_OPEN_SETTINGS=1
set WIDGET_EDITOR_PREVIEW_MS=120000
release\widget_regression.exe customEditorManualVisualCheck -o custom-editor-settings-manual.txt,txt
set WIDGET_EDITOR_OPEN_SETTINGS=
set WIDGET_EDITOR_PREVIEW_MS=
```

模式 `1` 由真实鼠标点击设置中的工作室入口。模式 `2` 在 500 ms 后程序调用该入口，适用于自动化工具无法列出设置工具窗时；工作室显示后的按钮、输入框和预览仍需真实鼠标验证，不能代替入口本身的点击验证。时限最长 180 秒，超时会清理未保存状态并关闭工作室与设置。截图输出至 `artifacts/custom-editor-settings-start.png` 和 `artifacts/custom-editor-from-settings.png`。

审查跳过字符串中的网址，检查自有 C++、GLSL 和回归源码中的英文注释、英文句子与遗留待办标记。注释的语义和必要性仍需人工审查。

本次新增重点用例：

- `clearTypeUsesRgbCoverageAndRestoresGlPainter` 检查真实 RGB 子像素覆盖和 GL 绘制状态恢复。
- `previewDoesNotStartServicesOrCreateConfiguration` 检查普通图库预览不启动卡片后台任务、不提前创建配置。
- `scalePreviewAnimatesWithoutBlockingControls` 检查中间尺寸、界面定时器延迟和向导表面尺寸稳定。
- `settingsAndWizardShareScaleSelector` 检查设置复用缩放控件并保存连续比例。
- `unifiedGlassPreservesBackgroundWithoutMasks` 检查菜单、向导、图库和卡片共用固定材质，并以恒色背景验证菜单与向导没有暗色遮罩或整面明暗渐变。
- `settingsJsonMigratesAndPreservesLayout` 同时验证旧模糊、透明度和系统模糊配置被清理，已有布局与其他偏好保持有效。
- `firstRunWizardShowsActualSizeWithoutResizingSurface` 对照浮动预览与真实桌面尺寸。
- `musicCardsRenderStatesAndRestoreConfiguration` 检查音乐布局、播放/暂停状态和配置恢复。
- `systemMediaReadsAndControlsOwnSession` 创建测试专用 Windows 媒体会话，验证标题、艺术家、时间线、暂停与切歌；不播放音频，不控制用户已有会话。系统没有发布测试会话或选择其他播放源时跳过该用例。

可通过 `QT_SCREEN_SCALE_FACTORS` 为独立测试进程指定 1、1.25、1.5、1.75、2 等 DPR，检查不同像素密度。必须对照测试输出确认实际 DPR；部分 Qt 配置对数值 1 沿用系统缩放，此时可用 `QT_SCALE_FACTOR` 校准。两个变量都只影响测试进程，不修改 Windows 显示设置。此次验证记录见 [review-2026-10-11.md](review-2026-10-11.md)。

`specializedBlurMatchesFallbackAndBoundsCache` 在五档画质、四种模糊半径下比较固定采样程序与回退程序的全部像素，并检查程序缓存上限和上下文资源释放。它随完整回归运行。

`renderingPipelineParityAndTiming` 是手动基准，不设置输出路径时跳过。测量原生帧缓冲中的玻璃、每帧更新模糊缓存、八段彩色半透明文字三种情况；每帧等待 GPU 完成，预热十帧后记录四十八帧的中位数和 P95。使用相同机器、DPR、构建配置对照两个版本，并交替重复运行，减少驱动预热和系统负载影响。

```bat
set WIDGET_PIPELINE_OUTPUT=artifacts\pipeline-before
release\widget_regression.exe renderingPipelineParityAndTiming -o pipeline-before.txt,txt
rem 使用修改前构建生成基线，再构建修改后版本执行以下命令。
set WIDGET_PIPELINE_BASELINE=artifacts\pipeline-before
set WIDGET_PIPELINE_OUTPUT=artifacts\pipeline-after
release\widget_regression.exe renderingPipelineParityAndTiming -o pipeline-after.txt,txt
set WIDGET_PIPELINE_OUTPUT=
set WIDGET_PIPELINE_BASELINE=
```

画质比较覆盖 0.60、0.80、1.00、1.25、1.50 五档；玻璃、模糊和文字路径均要求逐像素零差异。图像保存在指定目录，可直接用于修改前后视觉检查。结果记录见 [cleanup-performance-2026-10-11.md](cleanup-performance-2026-10-11.md)。

统一液态玻璃样式有意移除了旧的明暗渐变与额外高光，因此跨样式版本的旧图不能用作零差异基线；同一样式下的管线优化和回退程序仍要求逐像素一致。
