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
class WidgetRegression;

namespace ClearTypeText {
void setEnabled(bool enabled);
bool enabled();
// macOS 风格单通道面积覆盖率；Qt 仅负责整形和提供矢量轮廓。
QImage coverage(const QFont& font, const QString& text, QPoint* offset = nullptr);
bool draw(QPainter& painter, const QPointF& baseline, const QString& text);
// 普通控件与 GL 共用字形缓存，统计用于验证控件确实经过统一入口。
quint64 rasterDrawCount();

// 每个 GL 窗口独立拥有纹理和着色器；必须在所属上下文当前时释放。
class Renderer final
{
public:
    void begin(QPaintDevice* device, const QSize& nativeSize);
    void end();
    void clear();
    bool paint(QPainter& painter, const QPointF& baseline, const QString& text);
private:
    friend class ::WidgetRegression;
    void clearMasks();
    struct Mask {
        QImage pixels;
        QSize size;
        QPoint offset;
        unsigned int texture = 0;
    };
    std::unique_ptr<QOpenGLShaderProgram> m_shader;
    QCache<QString, Mask> m_masks{4096};
    unsigned int m_vertexBuffer = 0;
    int m_viewportLocation = -1;
    int m_rectangleLocation = -1;
    int m_maskTextureLocation = -1;
    int m_inkLocation = -1;
    QPaintDevice* m_device = nullptr;
    QSize m_nativeSize;
};
}
