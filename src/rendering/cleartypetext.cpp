#include "rendering/cleartypetext.h"
#include "rendering/renderdiagnostics.h"

#include <QFontMetricsF>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QVector2D>
#include <QVector4D>

namespace ClearTypeText {
namespace {
bool s_enabled = true;
thread_local Renderer* s_activeRenderer = nullptr;
}

void setEnabled(bool enabled) { s_enabled = enabled; }
bool enabled() { return s_enabled; }

QImage coverage(const QFont& font, const QString& text, QPoint* offset)
{
    const QRect bounds = QFontMetricsF(font).boundingRect(text).toAlignedRect().adjusted(-2,-2,2,2);
    if (offset)
        *offset = bounds.topLeft();
    if (bounds.isEmpty() || qint64(bounds.width()) * bounds.height() > 1024 * 1024)
        return {};
    // Windows 字体引擎仅在不透明 RGB 目标上保留 ClearType 三通道覆盖率。
    // 白字黑底直接得到覆盖掩码，避免透明 QImage 自动退回灰阶。
    QImage result(bounds.size(), QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(-QPointF(bounds.topLeft()), text);
    return result;
}

bool draw(QPainter& painter, const QPointF& baseline, const QString& text)
{
    return enabled() && s_activeRenderer && s_activeRenderer->paint(painter, baseline, text);
}

void Renderer::begin(QPaintDevice* device, const QSize& nativeSize)
{
    m_device = device;
    m_nativeSize = nativeSize;
    s_activeRenderer = this;
}

void Renderer::end()
{
    s_activeRenderer = nullptr;
    m_device = nullptr;
}

void Renderer::clear()
{
    auto* context = QOpenGLContext::currentContext();
    if (!context)
        return;
    auto* gl = context->extraFunctions();
    // 纹理存放在图像缓存之外统一释放，驱逐缓存时也通过同一上下文销毁。
    const auto keys = m_masks.keys();
    for (const QString& key : keys) {
        if (auto* mask = m_masks.object(key))
            gl->glDeleteTextures(1, &mask->texture);
    }
    m_masks.clear();
    if (m_vertexBuffer)
        gl->glDeleteBuffers(1, &m_vertexBuffer);
    m_vertexBuffer = 0;
    m_shader.reset();
}

bool Renderer::paint(QPainter& painter, const QPointF& baseline, const QString& text)
{
    if (painter.device() != m_device || !QOpenGLContext::currentContext()
        || painter.pen().brush().style() != Qt::SolidPattern)
        return false;
    const auto transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0
        || !qFuzzyCompare(transform.m11(), transform.m22()))
        return false;
    const QFont font = painter.font();
    if (font.styleStrategy() & (QFont::NoAntialias | QFont::NoSubpixelAntialias))
        return false;
    QFont rasterFont(font);
    const qreal logicalPixels = font.pixelSize() > 0 ? font.pixelSize()
        : font.pointSizeF() * painter.device()->logicalDpiY() / 72.0;
    rasterFont.setPixelSize(qMax(1, qRound(logicalPixels * transform.m11())));
    if (font.letterSpacingType() == QFont::AbsoluteSpacing)
        rasterFont.setLetterSpacing(QFont::AbsoluteSpacing, font.letterSpacing() * transform.m11());
    rasterFont.setWordSpacing(font.wordSpacing() * transform.m11());
    const QString key = rasterFont.key() + QChar('|') + text;
    Mask* mask = m_masks.object(key);
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    if (!mask) {
        auto created = std::make_unique<Mask>();
        created->pixels = coverage(rasterFont, text, &created->offset).convertToFormat(QImage::Format_RGBA8888);
        if (created->pixels.isNull())
            return false;
        // 先保留 CPU 掩码，纹理在 QPainter 切回原生绘制之后上传。
        const int cost = qMax(1, int(created->pixels.sizeInBytes() / 1024));
        if (cost > m_masks.maxCost())
            return false;
        if (m_masks.totalCost() + cost > m_masks.maxCost()) {
            // QCache 的自动驱逐不会释放 GL 纹理；先统一清空，保持缓存内存有界。
            const auto keys = m_masks.keys();
            for (const auto& cachedKey : keys) {
                auto* cached = m_masks.object(cachedKey);
                gl->glDeleteTextures(1, &cached->texture);
            }
            m_masks.clear();
        }
        mask = created.release();
        m_masks.insert(key, mask, cost);
    }
    const QPointF deviceBaseline = transform.map(baseline);
    const QPoint origin(qRound(deviceBaseline.x()) + mask->offset.x(),
                        qRound(deviceBaseline.y()) + mask->offset.y());
    const QRect destination(origin, mask->pixels.size());
    QRect clip(QPoint(), m_nativeSize);
    if (painter.hasClipping())
        clip &= transform.mapRect(painter.clipBoundingRect()).toAlignedRect();
    if (!clip.intersects(destination))
        return true;

    const QColor color = painter.pen().color();
    const float opacity = float(color.alphaF() * painter.opacity());
    painter.beginNativePainting();
    if (!m_shader) {
        m_shader = std::make_unique<QOpenGLShaderProgram>();
        const char* vertex = R"(
            #version 120
            attribute vec2 position;
            uniform vec2 viewport;
            uniform vec4 rectangle;
            varying vec2 uv;
            void main() {
                uv = position;
                vec2 pixel = rectangle.xy + position * rectangle.zw;
                gl_Position = vec4(pixel.x / viewport.x * 2.0 - 1.0,
                                   1.0 - pixel.y / viewport.y * 2.0, 0, 1);
            }
        )";
        const char* fragment = R"(
            #version 120
            varying vec2 uv;
            uniform sampler2D maskTexture;
            uniform vec4 ink;
            uniform int channel;
            void main() {
                vec3 coverage = texture2D(maskTexture, uv).rgb;
                float alpha = (channel == 3 ? max(max(coverage.r, coverage.g), coverage.b)
                                           : coverage[channel]) * ink.a;
                gl_FragColor = vec4(ink.rgb * alpha, alpha);
            }
        )";
        m_shader->bindAttributeLocation("position", 0);
        if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !m_shader->link()) {
            RenderDiagnostics::write("text.cleartype.failed", m_shader->log(), 10000);
            m_shader.reset();
            painter.endNativePainting();
            return false;
        }
        gl->glGenBuffers(1, &m_vertexBuffer);
        gl->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
        const float vertices[] = {0,0, 1,0, 0,1, 1,1};
        gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl->glEnableVertexAttribArray(0);
    }
    gl->glActiveTexture(GL_TEXTURE0);
    if (!mask->texture) {
        gl->glGenTextures(1, &mask->texture);
        gl->glBindTexture(GL_TEXTURE_2D, mask->texture);
        // Qt 的图像上传可能遗留 PBO、行长度或跳过偏移，不能用于这里的 CPU 指针。
        GLint unpackBuffer, rowLength, skipRows, skipPixels, alignment;
        gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        gl->glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
        gl->glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mask->pixels.width(), mask->pixels.height(),
                         0, GL_RGBA, GL_UNSIGNED_BYTE, mask->pixels.constBits());
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    } else {
        gl->glBindTexture(GL_TEXTURE_2D, mask->texture);
    }
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->glEnableVertexAttribArray(0);
    m_shader->bind();
    m_shader->setUniformValue("viewport", QVector2D(m_nativeSize.width(), m_nativeSize.height()));
    m_shader->setUniformValue("rectangle", QVector4D(destination.x(), destination.y(), destination.width(), destination.height()));
    m_shader->setUniformValue("maskTexture", 0);
    m_shader->setUniformValue("ink", QVector4D(color.redF(), color.greenF(), color.blueF(), opacity));
    gl->glViewport(0, 0, m_nativeSize.width(), m_nativeSize.height());
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(clip.x(), m_nativeSize.height() - clip.bottom() - 1, clip.width(), clip.height());
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // 分别混合 RGB 覆盖率，再用最大覆盖率更新 alpha；透明玻璃边缘仍保留正确透明度。
    for (int channel = 0; channel < 4; ++channel) {
        gl->glColorMask(channel == 0, channel == 1, channel == 2, channel == 3);
        m_shader->setUniformValue("channel", channel);
        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisableVertexAttribArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_shader->release();
    painter.endNativePainting();
    return true;
}
}
