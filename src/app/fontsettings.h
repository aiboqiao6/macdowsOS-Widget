#pragma once
#include <QFont>
#include <QByteArray>
#include <QString>
class QWidget;

// 全局字体注册与抗锯齿策略，不依赖具体卡片或窗口。
namespace AppFonts {
QString family();
int smoothing();
bool setSmoothing(int level);
void applySmoothing(QFont& font);
// Shared custom grayscale renderer; interface quality stays independent of cards.
void useInterfaceText(QWidget* window);
QFont displayFont(int pointSize, QFont::Weight weight = QFont::Normal);
QString webFontUrl();
QString webStyleSheet();
QByteArray webFontData();
}
