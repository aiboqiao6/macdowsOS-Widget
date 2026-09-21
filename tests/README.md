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
