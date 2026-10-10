#pragma once

#include <QString>

// 本地诊断日志有大小上限，不记录截图或组件内容。
namespace RenderDiagnostics {
bool initialize(const QString& directory = {});
void shutdown();
bool enabled();
QString logPath();
// 重复错误按键限频并汇总省略次数；成功帧每五秒汇总一次。
void write(const QString& event, const QString& details,
           int minimumIntervalMs = 0);
}
