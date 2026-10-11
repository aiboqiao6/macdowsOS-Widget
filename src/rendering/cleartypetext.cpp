#include "rendering/cleartypetext.h"
#include "rendering/renderdiagnostics.h"
#include "rendering/glpixeltransfer.h"

#include <QFontMetricsF>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QRegion>
#include <QVector2D>
#include <QVector4D>

namespace ClearTypeText {
namespace {
bool s_enabled = true;
thread_local Renderer* s_activeRenderer = nullptr;
thread_local quint64 s_rasterDraws = 0;

QFont nativeFont(const QPainter& painter)
{
    const QFont font = painter.font();
    QFont result(font);
    const qreal scale = painter.deviceTransform().m11();
    const qreal pixels = font.pixelSize() > 0 ? font.pixelSize()
        : font.pointSizeF() * painter.device()->logicalDpiY() / 72.0;
    result.setPixelSize(qMax(1, qRound(pixels * scale)));
    if (font.letterSpacingType() == QFont::AbsoluteSpacing)
        result.setLetterSpacing(QFont::AbsoluteSpacing, font.letterSpacing() * scale);
    result.setWordSpacing(font.wordSpacing() * scale);
    return result;
}

bool paintRaster(QPainter& painter, const QPointF& baseline, const QString& text)
{
    const auto transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0
        || !qFuzzyCompare(transform.m11(), transform.m22())
        || painter.pen().brush().style() != Qt::SolidPattern)
        return false;
    QFont font = nativeFont(painter);
    const bool rgb = enabled() && !(font.styleStrategy() & (QFont::NoAntialias | QFont::NoSubpixelAntialias));
    if (!rgb && !(font.styleStrategy() & QFont::NoAntialias))
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(font.styleStrategy() | QFont::NoSubpixelAntialias));
    QPoint offset;
    const QImage mask = coverage(font, text, &offset);
    if (mask.isNull())
        return false;
    const QPointF deviceBaseline = transform.map(baseline);
    const QPoint origin(qRound(deviceBaseline.x()) + offset.x(), qRound(deviceBaseline.y()) + offset.y());
    const QColor ink = painter.pen().color();
    const qreal opacity = ink.alphaF() * painter.opacity();

    // 图库原生像素画布直接按 RGB 覆盖率混合，使用与 GL 完全相同的字形及公式。
    auto* target = painter.device()->devType() == QInternal::Image
        ? static_cast<QImage*>(painter.device()) : nullptr;
    if (rgb && target && target->isDetached()
        && (target->format() == QImage::Format_RGB32 || target->format() == QImage::Format_ARGB32_Premultiplied)
        && painter.compositionMode() == QPainter::CompositionMode_SourceOver) {
        const QRect destination(origin, mask.size());
        QRegion clip(target->rect());
        if (painter.hasClipping())
            clip &= transform.map(painter.clipRegion());
        clip &= destination;
        for (const QRect& rect : clip) {
            for (int y = rect.top(); y <= rect.bottom(); ++y) {
                auto* pixels = reinterpret_cast<QRgb*>(target->scanLine(y));
                const auto* coverageRow = reinterpret_cast<const QRgb*>(mask.constScanLine(y - origin.y()));
                for (int x = rect.left(); x <= rect.right(); ++x) {
                    const QRgb sample = coverageRow[x - origin.x()];
                    const qreal ar = qRed(sample) / 255.0 * opacity;
                    const qreal ag = qGreen(sample) / 255.0 * opacity;
                    const qreal ab = qBlue(sample) / 255.0 * opacity;
                    const qreal alpha = qMax(ar, qMax(ag, ab));
                    const QRgb old = pixels[x];
                    pixels[x] = qRgba(qRound(ink.red() * ar + qRed(old) * (1 - ar)),
                                      qRound(ink.green() * ag + qGreen(old) * (1 - ag)),
                                      qRound(ink.blue() * ab + qBlue(old) * (1 - ab)),
                                      qRound(255 * alpha + qAlpha(old) * (1 - alpha)));
                }
            }
        }
    } else {
        // 透明控件不能取得目标背景，复用同一字形的灰阶覆盖并由 Qt 合成。
        const QString key = font.key() + QChar('|') + QString::number(ink.rgba(), 16) + QChar('|') + text;
        static thread_local QCache<QString, QImage> colored(8 * 1024);
        QImage* image = colored.object(key);
        std::unique_ptr<QImage> oversized;
        if (!image) {
            QImage pixels(mask.size(), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < pixels.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(pixels.scanLine(y));
                const auto* samples = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
                for (int x = 0; x < pixels.width(); ++x) {
                    const int alpha = qRound(qGray(samples[x]) * ink.alphaF());
                    row[x] = qRgba((ink.red() * alpha + 127) / 255, (ink.green() * alpha + 127) / 255,
                                   (ink.blue() * alpha + 127) / 255, alpha);
                }
            }
            image = new QImage(std::move(pixels));
            const int cost = qMax(1, int(image->sizeInBytes() / 1024));
            if (cost > colored.maxCost()) oversized.reset(image);
            else colored.insert(key, image, cost);
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(QRectF(transform.inverted().map(QPointF(origin)), QSizeF(image->size()) / transform.m11()), *image);
        painter.restore();
    }
    ++s_rasterDraws;
    return true;
}
}

void setEnabled(bool enabled) { s_enabled = enabled; }
bool enabled() { return s_enabled; }

QImage coverage(const QFont& font, const QString& text, QPoint* offset)
{
    struct Glyph { QImage mask; QPoint offset; };
    static thread_local QCache<QString, Glyph> cache(16 * 1024);
    const QString key = font.key() + QChar('|') + text;
    if (const auto* glyph = cache.object(key)) {
        if (offset) *offset = glyph->offset;
        return glyph->mask;
    }
    const QRect bounds = QFontMetricsF(font).boundingRect(text).toAlignedRect().adjusted(-2,-2,2,2);
    if (offset)
        *offset = bounds.topLeft();
    if (bounds.isEmpty() || qint64(bounds.width()) * bounds.height() > 1024 * 1024)
        return {};
    // RGB 模式保留系统三通道覆盖率；灰阶模式读取透明画布的原生 alpha。
    const bool grayscale = font.styleStrategy() & (QFont::NoSubpixelAntialias | QFont::NoAntialias);
    QImage result(bounds.size(), grayscale ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    result.fill(grayscale ? Qt::transparent : Qt::black);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(-QPointF(bounds.topLeft()), text);
    painter.end();
    if (grayscale) {
        QImage mask(result.size(), QImage::Format_RGB32);
        for (int y = 0; y < result.height(); ++y) {
            const auto* source = reinterpret_cast<const QRgb*>(result.constScanLine(y));
            auto* row = reinterpret_cast<QRgb*>(mask.scanLine(y));
            for (int x = 0; x < result.width(); ++x) {
                const int alpha = qAlpha(source[x]);
                row[x] = qRgb(alpha, alpha, alpha);
            }
        }
        result = std::move(mask);
    }
    const int cost = qMax(1, int(result.sizeInBytes() / 1024));
    if (cost <= cache.maxCost())
        cache.insert(key, new Glyph{result, bounds.topLeft()}, cost);
    return result;
}

bool draw(QPainter& painter, const QPointF& baseline, const QString& text)
{
    if (enabled() && s_activeRenderer && s_activeRenderer->paint(painter, baseline, text))
        return true;
    return paintRaster(painter, baseline, text);
}

quint64 rasterDrawCount() { return s_rasterDraws; }

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

void Renderer::clearMasks()
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
}

void Renderer::clear()
{
    auto* context = QOpenGLContext::currentContext();
    if (!context)
        return;
    clearMasks();
    auto* gl = context->extraFunctions();
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
    const QFont rasterFont = nativeFont(painter);
    const QString key = rasterFont.key() + QChar('|') + text;
    Mask* mask = m_masks.object(key);
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    if (!mask) {
        auto created = std::make_unique<Mask>();
        created->pixels = coverage(rasterFont, text, &created->offset).convertToFormat(QImage::Format_RGBA8888);
        if (created->pixels.isNull())
            return false;
        created->size = created->pixels.size();
        // 先保留 CPU 掩码，纹理在 QPainter 切回原生绘制之后上传。
        const int cost = qMax(1, int(created->pixels.sizeInBytes() / 1024));
        if (cost > m_masks.maxCost())
            return false;
        if (m_masks.totalCost() + cost > m_masks.maxCost()) {
            // QCache 的自动驱逐不会释放 GL 纹理；先统一清空，保持缓存内存有界。
            clearMasks();
        }
        mask = created.release();
        m_masks.insert(key, mask, cost);
    }
    const QPointF deviceBaseline = transform.map(baseline);
    const QPoint origin(qRound(deviceBaseline.x()) + mask->offset.x(),
                        qRound(deviceBaseline.y()) + mask->offset.y());
    const QRect destination(origin, mask->size);
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
        // 链接后缓存位置，避免每个字串和每个颜色通道重复查询驱动。
        m_viewportLocation = m_shader->uniformLocation("viewport");
        m_rectangleLocation = m_shader->uniformLocation("rectangle");
        m_maskTextureLocation = m_shader->uniformLocation("maskTexture");
        m_inkLocation = m_shader->uniformLocation("ink");
        m_channelLocation = m_shader->uniformLocation("channel");
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
        PixelTransferScope transfer(gl, true);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mask->pixels.width(), mask->pixels.height(),
                         0, GL_RGBA, GL_UNSIGNED_BYTE, mask->pixels.constBits());
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // 纹理上传是同步消费客户端像素，后续绘制只保留尺寸和 GPU 纹理。
        mask->pixels = {};
    } else {
        gl->glBindTexture(GL_TEXTURE_2D, mask->texture);
    }
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->glEnableVertexAttribArray(0);
    m_shader->bind();
    m_shader->setUniformValue(m_viewportLocation, QVector2D(m_nativeSize.width(), m_nativeSize.height()));
    m_shader->setUniformValue(m_rectangleLocation, QVector4D(destination.x(), destination.y(), destination.width(), destination.height()));
    m_shader->setUniformValue(m_maskTextureLocation, 0);
    m_shader->setUniformValue(m_inkLocation, QVector4D(color.redF(), color.greenF(), color.blueF(), opacity));
    gl->glViewport(0, 0, m_nativeSize.width(), m_nativeSize.height());
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(clip.x(), m_nativeSize.height() - clip.bottom() - 1, clip.width(), clip.height());
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // 分别混合 RGB 覆盖率，再用最大覆盖率更新 alpha，保持透明玻璃上的字形覆盖。
    for (int channel = 0; channel < 4; ++channel) {
        gl->glColorMask(channel == 0, channel == 1, channel == 2, channel == 3);
        m_shader->setUniformValue(m_channelLocation, channel);
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
