# 回归测试

设置窗口与更新检查定向回归：

```bat
release\widget_regression.exe updatesCompareOnlyFirstEightDigits ^
  updatesHandleUnsortedDraftAndInvalidResponses ^
  updatesRecoverFromNetworkFailureAndPreventDuplicateRequests ^
  settingsNavigationAndUpdatePopupShowTitleAndNotes ^
  settingsPagesRemainUsableWithoutFooterAtCompactSize settingsSidebarFollowsWindowBehindIt ^
  settingsControlsMatchPaintedHitAreasAndApplyLive settingsScaleDragFlushesOnClose rightClickMenusOmitAbout ^
  defaultMacTextMigratesRetiredSmoothingChoice ^
  settingsScaleFollowsSliderAndFitsScreen settingsTrafficIsFlatAndRespondsToPress settingsFocusOutlinesFollowActualControls ^
  settingsColorModeAppliesImmediatelyAndSurvivesClose imageAntialiasingSettingsApplyImmediatelyAndRefreshGallery ^
  settingsTextIsIndependentOfWidgetPreferences settingsBlurPreservesPixelsAndBoundsWork ^
  settingsLayoutCacheTracksFontAndNewControls settingsClockStopsWhileMinimizedAndRestarts ^
  settingsSidebarStaysLiveWhileDraggingAtIndependentResolution settingsVSyncKeepsFramesDuringContinuousMovement ^
  settingsAndWizardShareScaleSelector materialSettingsApplyImmediatelyAndSurviveEscape ^
  materialSettingsPersistAndResetToOriginalDefaults ^
  customSettingsOpensUsableEditorAndRetainsLiveSettings -o settings-results.txt,txt
```

更新检查只比较发布 Tag 与本地 `BuildId` 的前 8 位日期；同日不同后缀不提示更新，较旧日期也不提示。发布列表包含 Beta，忽略草稿与无效日期。更新弹窗显示发布标题与完整日志，并提供 GitHub 发布页入口。联网用例注入异步响应，不访问外网；玻璃用例通过改变下层窗口颜色验证实时采样。界面截图保存到 `artifacts/settings-*.png`。

设置项调整后立即应用并自动保存，关闭窗口或按 Esc 保留修改。即时应用用例覆盖开关两端点击、键盘操作、下拉菜单、数字输入与步进按钮、缩放拖动末值，以及托盘菜单移除“关于”。亮暗主题切换后，导航高度和数字控件点击区域应保持一致。字体灰阶抗锯齿默认开启，旧的关闭设置会在启动时迁移为开启；用例验证设置页不再显示“字体抗锯齿”和“文字显示”。测试不会修改 Windows 自启动注册表。

设置页跟随小组件缩放比例调整字体、间距、控件和窗口大小，并限制在屏幕可用空间内。拖动时桌面组件实时缩放，设置页在松开滑块后调整，避免移动正在操作的滑块。缩放用例覆盖 40%、75%、125%、200% 以及亮暗主题；交互检查覆盖红绿灯纯色填充、鼠标与键盘按下反馈、圆形点击区域，以及开关、数字框、选择器和滑块的焦点轮廓。

设置窗口使用独立的灰阶字体、侧栏捕获分辨率与刷新节奏。文字用例逐档切换小组件抗锯齿和渲染比例，比较标题、导航和选择器的像素结果；背景用例切换黑白背景，检查字色保持一致。模糊用例与原六遍算法逐像素对照，覆盖单像素边界，并检查异步任务合并、隐藏时丢弃结果和销毁安全。测试输出模糊处理时间和连续拖动的更新间隔，用于同机对照；不将单次计时作为跨机器性能保证。

`settingsLayoutCacheTracksFontAndNewControls` 检查字体变化、运行时新增和删除控件后的布局与主题更新；`settingsClockStopsWhileMinimizedAndRestarts` 检查最小化时线程停止、恢复显示后继续工作。单独运行 `settingsWindowPerformance` 可测量完整设置页的缩放布局、背景响应、240×600 模糊耗时，以及静止和最小化时的进程 CPU 时间与内存；内存和 CPU 包含测试进程及共享采集组件，结果用于同机对照，不作为稳定阈值。

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
  customSettingsOpensUsableEditorAndRetainsLiveSettings ^
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

- `macTextUsesGrayscaleCoverageAndRestoresGlPainter` 检查单通道灰阶覆盖、无彩边与 GL 绘制状态恢复。
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

玻璃中央不叠加整面明暗遮罩，轮廓保留方向性高光和细暗边。更换材质或字形栅格算法后，旧图不能用作零差异基线；同一样式下的管线优化和回退程序仍要求逐像素一致。

多组件首帧、句柄重建与捕获调度定向检查：

```bat
release\widget_regression.exe ^
  firstShowSeedsEveryGlassBackdrop sceneRepairsMissingBlurTargetAndReusesValidTargets ^
  completeCaptureFramesAvoidFallbackAllocation duplicateNativeRegistrationDoesNotDiscardBackdrop ^
  queuedBackdropRequestsCoalesceWithoutLosingCompletions partialCaptureStillPreservesFallbackAndCoverage ^
  -o startup-probes.txt,txt
```

覆盖五档首帧背景、缺失模糊目标的自动恢复、有效目标复用、完整帧共享、部分捕获补洞、重复原生句柄通知及请求合并后的回调计数。修改前后检查、225 张图像的逐像素对比和持续捕获结果见 [startup-performance-2026-10-13.md](startup-performance-2026-10-13.md)。

WGC 工具窗口异常定向回归：

```bat
release\widget_regression.exe ^
  captureSkipsUnsupportedToolWindows wizardQualityPopupIsExcludedFromCapture ^
  liveBackdropAndStableCapture stationaryLiveBackdropFollowsAnimation ^
  captureSurvivesResizeRecreationAndCancelledReceivers -o capture-results.txt,txt
```

`captureSkipsUnsupportedToolWindows` 检查未带 `WS_EX_APPWINDOW` 的工具窗口在调用 WGC 前被拒绝、其覆盖区域保留回退、普通窗口仍能提供有效像素，以及带 `APPWINDOW` 的工具窗口仍可捕获。同时通过异常监视器要求 `GraphicsCapture.dll` 内部的 C++ 首次机会异常计数为零。捕获源测试窗口使用普通窗口样式，以免把 WGC 不支持的工具窗口当作有效来源。桌面宿主仍保留在拓扑中，捕获不可用时交由壁纸补齐。

自研字体栅格与边缘高光检查：

```bat
release\widget_regression.exe ^
  customRasterizerPreservesCountersAndMixedScripts glassRimHighlightsStayVisibleInBothThemes ^
  macTextUsesGrayscaleCoverageAndRestoresGlPainter unifiedTextMatchesNativeImageAndGlCoverage ^
  customTextRetainsNativeCoverageAtFractionalScale weatherReferenceWithCustomTextAndRim ^
  -o appearance-results.txt,txt
```

字形测试验证空洞、半粗体、单通道灰阶覆盖率、中英文及复杂文字整形、分数缩放下的原生分辨率和 CPU/GL 合成一致性。高光检查覆盖亮暗模式和三个画质档位，要求上下边缘反光可见、中央颜色稳定、圆角外透明。可配合 `QT_SCREEN_SCALE_FACTORS=1、1.25、1.5、2` 检查像素落点。实现与验证范围见 [custom-text-rim-review-2026-10-22.md](custom-text-rim-review-2026-10-22.md)。

`customTextCoversEditorsSelectionsAndRichDocuments` 检查输入框、数字框、编辑器、列表和富文本经过同一栅格器，并覆盖选区、撤销和 IME 组合文字。`customTextEnforcesPingFangAndHandlesTransforms` 检查字体归一、旋转、非等比缩放、渐变画笔及裁剪。`textAdapterScansAtFinalDeviceResolution` 对照直接绘制与适配绘制在四种 DPR 下的最终像素；`customHtmlLoadsBundledPingFangOffline` 验证浏览器离线字体加载，以及元数据修复前后字形与横向字距一致。旧的非零抗锯齿档位统一兼容到 macOS 灰阶；`defaultMacTextMigratesRetiredSmoothingChoice` 验证默认开启和旧配置迁移，设置页已移除字体选项与说明行。架构及 WebView2 边界见 [字体渲染说明](../docs/text-rendering.md)。
