#pragma once

#include <QCache>
#include <QColor>
#include <QImage>
#include <QOpenGLShaderProgram>
#include <QPointF>
#include <QRect>
#include <memory>

class QPainter;
class QPaintDevice;
class QFont;

namespace ClearTypeText {
void setEnabled(bool enabled);
bool enabled();
// 公开掩码生成供回归测试验证原生分辨率和 RGB 子像素覆盖率。
QImage coverage(const QFont& font, const QString& text, QPoint* offset = nullptr);
bool draw(QPainter& painter, const QPointF& baseline, const QString& text);

// 每个 GL 窗口独立拥有纹理和着色器；必须在所属上下文当前时释放。
class Renderer final
{
public:
    void begin(QPaintDevice* device, const QSize& nativeSize);
    void end();
    void clear();
    bool paint(QPainter& painter, const QPointF& baseline, const QString& text);
private:
    void clearMasks();
    struct Mask {
        QImage pixels;
        QPoint offset;
        unsigned int texture = 0;
    };
    std::unique_ptr<QOpenGLShaderProgram> m_shader;
    QCache<QString, Mask> m_masks{4096};
    unsigned int m_vertexBuffer = 0;
    QPaintDevice* m_device = nullptr;
    QSize m_nativeSize;
};
}
