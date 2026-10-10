#pragma once

#include <QString>

// Local, bounded diagnostics. No screenshots or widget payloads are stored.
namespace RenderDiagnostics {
bool initialize(const QString& directory = {});
void shutdown();
bool enabled();
QString logPath();
// Repeated errors share a key and are throttled; the next record includes the
// number suppressed. Successful render frames are summarized every 5 seconds.
void write(const QString& event, const QString& details,
           int minimumIntervalMs = 0);
}
