#pragma once
#include <QFont>
#include <QString>

// 全局字体注册与抗锯齿策略，不依赖具体卡片或窗口。
namespace AppFonts {
QString family();
int smoothing();
bool setSmoothing(int level);
void applySmoothing(QFont& font);
QFont displayFont(int pointSize, QFont::Weight weight = QFont::Normal);
}
