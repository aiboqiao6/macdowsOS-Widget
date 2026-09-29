# 静止层级与右键菜单修复

静止卡片位于普通应用下方、普通壁纸宿主上方。实际拖动时仍可以临时前置，结束/取消拖动后恢复桌面层。小组件库继续使用独立的前台面板。

## 修复

- 之前只依据 WS_EX_TOPMOST 和初始化标记判断层级，漏掉普通窗口层内的前移。现在同时检查相对于普通应用、其他桌面卡片和壁纸宿主的位置。
- 之前置底先无条件执行 HWND_NOTOPMOST，可能先把普通卡片提到普通窗口最前，再向下移动。现在直接使用桌面层插入位置，避免这个中间状态。
- 在 WM_WINDOWPOSCHANGING 中纠正静止卡片的前移/置顶请求，点击返回 MA_NOACTIVATE。不会改写拖动卡片或小组件库的层级。捕获开关不再决定是否维护桌面层。
- 原生窗口拓扑变化会触发检查；复用已有 200 ms 监测定时器处理绕过 WINDOWPOSCHANGING 或没有可靠重排通知的调用。层级正确时不调用 SetWindowPos，防止多个静止组件不断互相调整位置。
- 所有卡片的右键菜单增加“添加小组件…”。通过托盘主卡片复用同一个组件库，避免每张卡片创建一个独立捕获面板。
- 修正非天气卡片取消菜单时 selected 和 cityAction 同为 nullptr，误触发城市搜索的问题。
- 右键菜单改为真正的液态玻璃表面：复用卡片的模糊/折射管线、渲染质量和文字抗锯齿设置，增加圆角高光、悬停层、添加/搜索/删除图标和禁用态。
- 菜单保留 QMenu 的键盘导航、鼠标命中、点击外部关闭、Esc 关闭和无障碍行为；菜单关闭后立即停止渲染与采样。
- 实时背景开启时菜单按显示器刷新策略更新；关闭时只取一次背景。Windows 的 SysShadow 装饰窗口不再作为黑色背景参与采样。
- 修复 AMD 驱动中 WGC/D3D 线程退出时可能发生的等待：空闲时在线程仍存活的情况下释放采集流，避免菜单反复打开后界面卡死。

原生消息处理参考 [WM_WINDOWPOSCHANGING](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanging)、[SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)。前者明确指出在该消息内修改 SWP_NOACTIVATE / SWP_NOOWNERZORDER 会被忽略，因此实现只修改插入锚点；激活策略通过窗口属性和鼠标激活消息处理。

## 验证场景

- 对静止卡片执行原生 HWND_TOP、HWND_TOPMOST 和 Qt raise，立即检查仍位于普通应用下方。
- 使用 SWP_NOSENDCHANGING 绕过前置消息，检查捕获已关闭时仍能恢复桌面层。
- 隐藏再显示、拖动结束、拖动取消、壁纸宿主顺序，以及两张静止卡片是否出现反复重排。
- 天气与时钟的实际右键菜单：取消不打开城市对话框，选择添加能打开组件库；非主卡片重复打开时复用主卡片的组件库。
- CPU/GPU 两种菜单渲染、实时背景变化、禁用项、鼠标/键盘选择、点击外部关闭、屏幕边缘定位、反复创建销毁，以及静止时不持续重绘。

专项日志位于 `obj/regression/glass-menu-final.txt`。应用继续编译到 VS 默认 `x64/Release/` 和 `x64/Debug/`。

最终完整回归 **71 通过、0 失败**，日志为 `obj/regression/glass-menu-full.txt`。Release、Debug 均编译成功，构建日志无错误/警告，差异空白检查通过。
