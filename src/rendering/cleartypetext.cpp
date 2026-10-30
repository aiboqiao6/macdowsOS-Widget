#include "rendering/cleartypetext.h"
#include "rendering/renderdiagnostics.h"
#include "rendering/glpixeltransfer.h"
#include "rendering/textpaintdevice.h"
#include "app/fontsettings.h"

#include <QFontMetricsF>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QGlyphRun>
#include <QRawFont>
#include <QRegion>
#include <QTextLayout>
#include <QVector2D>
#include <QVector4D>
#include <algorithm>
#include <cmath>

namespace ClearTypeText {
namespace {
bool s_enabled = true;
thread_local Renderer* s_activeRenderer = nullptr;
thread_local quint64 s_rasterDraws = 0;

const QImage& metricDevice()
{
    static thread_local const QImage device = [] {
        QImage result(1, 1, QImage::Format_Grayscale8);
        result.setDotsPerMeterX(3780); result.setDotsPerMeterY(3780); // logical 96 DPI
        return result;
    }();
    return device;
}

QPainterPath textOutline(const QFont& font, const QString& text)
{
    // Qt 只负责字体选择、字距与复杂文字整形；轮廓覆盖率由下方扫描器计算。
    QTextLayout layout(text, font, const_cast<QImage*>(&metricDevice()));
    layout.beginLayout();
    const QTextLine line = layout.createLine();
    layout.endLayout();
    QPainterPath outline;
    outline.setFillRule(Qt::WindingFill);
    if (!line.isValid())
        return outline;
    for (const QGlyphRun& run : layout.glyphRuns()) {
        const QRawFont raw = run.rawFont();
        const QByteArray weightTable = raw.fontTable("OS/2");
        const int faceWeight = weightTable.size() >= 6
            ? (quint8(weightTable[4]) << 8) | quint8(weightTable[5]) : raw.weight();
        const qreal embolden = raw.pixelSize() * qMax(0, int(font.weight()) - faceWeight) / 8500.0;
        QPainterPathStroker stroke;
        stroke.setWidth(embolden);
        stroke.setJoinStyle(Qt::RoundJoin);
        const auto indexes = run.glyphIndexes();
        const auto positions = run.positions();
        static thread_local QCache<QString, QPainterPath> outlines(2 * 1024);
        const QString face = raw.familyName() + QChar('|') + raw.styleName()
            + QChar('|') + QString::number(raw.pixelSize(), 'g', 12)
            + QChar('|') + QString::number(font.weight()) + QChar('|')
            + QString::number(font.italic()) + QChar('|');
        for (qsizetype i = 0; i < indexes.size(); ++i) {
            const QString key = face + QString::number(indexes[i]);
            QPainterPath glyph;
            if (const auto* cached = outlines.object(key)) glyph = *cached;
            else {
                glyph = raw.pathForGlyph(indexes[i]);
                if (font.italic() && raw.style() == QFont::StyleNormal) {
                    QTransform italic;
                    italic.shear(-.2, 0);
                    glyph = italic.map(glyph);
                }
                // 字体文件只有常规字重时补足轮廓厚度，避免提取轮廓后丢失半粗体。
                if (embolden > 0 && !glyph.isEmpty())
                    glyph = glyph.united(stroke.createStroke(glyph));
                const int cost = qMax(1, int((glyph.elementCount() * sizeof(QPainterPath::Element)
                    + key.size() * sizeof(QChar) + 1023) / 1024));
                if (cost <= outlines.maxCost()) outlines.insert(key, new QPainterPath(glyph), cost);
            }
            glyph.translate(positions[i] - QPointF(0, line.ascent()));
            outline.addPath(glyph);
        }
    }
    const QFontMetricsF metrics(font, &metricDevice());
    const qreal width = line.horizontalAdvance();
    const qreal thickness = qMax(qreal(.5), metrics.lineWidth());
    if (font.underline()) outline.addRect(QRectF(0, metrics.underlinePos(), width, thickness));
    if (font.overline()) outline.addRect(QRectF(0, -metrics.ascent(), width, thickness));
    if (font.strikeOut()) outline.addRect(QRectF(0, -metrics.strikeOutPos(), width, thickness));
    return outline;
}

QImage rasterizeOutline(const QPainterPath& outline, const QRect& bounds, bool aliased)
{
    constexpr int verticalSamples = 16;
    struct Edge { qreal x, y, endY, slope; int winding; };
    struct Crossing { qreal x; int winding; };
    QVector<Edge> edges;
    QTransform samples;
    samples.scale(1, verticalSamples);
    samples.translate(-bounds.left(), -bounds.top());
    // 在采样网格内细分曲线；不调用系统的字形位图或 QPainter 文字栅格器。
    for (const QPolygonF& polygon : outline.toSubpathPolygons(samples)) {
        for (qsizetype i = 0; i < polygon.size(); ++i) {
            QPointF from = polygon[i], to = polygon[(i + 1) % polygon.size()];
            if (qFuzzyCompare(from.y(), to.y()))
                continue;
            const int winding = from.y() < to.y() ? 1 : -1;
            if (from.y() > to.y()) std::swap(from, to);
            edges.append({from.x(), from.y(), to.y(),
                          (to.x() - from.x()) / (to.y() - from.y()), winding});
        }
    }
    const int width = bounds.width();
    QVector<float> row(width);
    QVector<Crossing> crossings;
    crossings.reserve(edges.size());
    QImage mask(bounds.size(), QImage::Format_Grayscale8);
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y < b.y; });
    QVector<Edge> active;
    qsizetype nextEdge = 0;
    for (int y = 0; y < bounds.height(); ++y) {
        std::fill(row.begin(), row.end(), 0.0f);
        for (int sample = 0; sample < verticalSamples; ++sample) {
            const qreal scanY = y * verticalSamples + sample + .5;
            crossings.clear();
            while (nextEdge < edges.size() && edges[nextEdge].y <= scanY)
                active.append(edges[nextEdge++]);
            active.erase(std::remove_if(active.begin(), active.end(),
                [scanY](const Edge& edge) { return edge.endY <= scanY; }), active.end());
            for (const Edge& edge : active)
                crossings.append({edge.x + (scanY - edge.y) * edge.slope, edge.winding});
            std::sort(crossings.begin(), crossings.end(),
                [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
            int winding = 0;
            qreal start = 0;
            for (const Crossing& crossing : crossings) {
                const int next = winding + crossing.winding;
                if (winding == 0 && next != 0) start = crossing.x;
                if (winding != 0 && next == 0) {
                    const qreal left = qMax<qreal>(0, start);
                    const qreal right = qMin<qreal>(width, crossing.x);
                    for (int x = qFloor(left); x < qCeil(right); ++x) {
                        // 横向精确积分像素交集，纵向八次采样，保留细笔画的面积。
                        row[x] += float(qMax<qreal>(0, qMin(right, qreal(x + 1))
                            - qMax(left, qreal(x)))) / verticalSamples;
                    }
                }
                winding = next;
            }
        }
        auto* pixels = mask.scanLine(y);
        for (int x = 0; x < bounds.width(); ++x) {
            // One area coverage for all channels, as on modern macOS. Analytic
            // horizontal integration preserves small strokes without color halos.
            pixels[x] = uchar(aliased ? (row[x] >= .5f ? 255 : 0)
                                     : qBound(0, qRound(row[x] * 255), 255));
        }
    }
    return mask;
}

QFont nativeFont(const QPainter& painter)
{
    const QFont font = painter.font();
    QFont result(font);
    result.setFamily(AppFonts::family());
    const qreal scale = painter.deviceTransform().m11();
    const qreal pixels = font.pixelSize() > 0 ? font.pixelSize()
        : font.pointSizeF() * painter.device()->logicalDpiY() / 72.0;
    // Fractional sizes preserve Qt's advances and keep editor cursors aligned.
    result.setPointSizeF(qMax(qreal(.1), pixels * scale) * 72 / 96);
    if (font.letterSpacingType() == QFont::AbsoluteSpacing)
        result.setLetterSpacing(QFont::AbsoluteSpacing, font.letterSpacing() * scale);
    result.setWordSpacing(font.wordSpacing() * scale);
    return result;
}

QImage tinted(const QImage& mask, const QColor& ink)
{
    QImage pixels(mask.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < pixels.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(pixels.scanLine(y));
        const auto* samples = mask.constScanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const int alpha = qRound(samples[x] * ink.alphaF());
            row[x] = qRgba((ink.red() * alpha + 127) / 255, (ink.green() * alpha + 127) / 255,
                           (ink.blue() * alpha + 127) / 255, alpha);
        }
    }
    return pixels;
}

bool paintTransformed(QPainter& painter, const QPointF& baseline, const QString& text)
{
    const QTransform transform = painter.deviceTransform();
    if (!transform.isInvertible()) return true;
    QFont font = painter.font();
    font.setFamily(AppFonts::family());
    const qreal pixels = font.pixelSize() > 0 ? font.pixelSize()
        : font.pointSizeF() * painter.device()->logicalDpiY() / 72;
    font.setPointSizeF(qMax(qreal(.1), pixels) * 72 / 96);
    const QPainterPath outline = transform.map(textOutline(font, text).translated(baseline));
    QRect bounds = outline.boundingRect().toAlignedRect().adjusted(-2, -2, 2, 2);
    // Bound long editor lines and transformed glyphs to the visible device.
    bounds &= transform.mapRect(QRectF(painter.window())).toAlignedRect();
    if (painter.hasClipping())
        bounds &= transform.mapRect(painter.clipBoundingRect()).toAlignedRect();
    if (bounds.isEmpty()) return true;
    // Tiles keep allocation bounded even for printing or unusually large text.
    for (int y = bounds.top(); y <= bounds.bottom(); y += 256) {
        for (int x = bounds.left(); x <= bounds.right(); x += 512) {
            const QRect tile = QRect(x, y, 512, 256).intersected(bounds);
            const QImage mask = rasterizeOutline(outline, tile, font.styleStrategy() & QFont::NoAntialias);
            QImage pixels;
            if (painter.pen().brush().style() == Qt::SolidPattern)
                pixels = tinted(mask, painter.pen().color());
            else {
                pixels = QImage(tile.size(), QImage::Format_ARGB32_Premultiplied);
                pixels.fill(Qt::transparent);
                QPainter brush(&pixels);
                brush.setTransform(transform * QTransform::fromTranslate(-tile.x(), -tile.y()));
                brush.setBrushOrigin(painter.brushOrigin());
                brush.fillRect(transform.inverted().mapRect(QRectF(tile)), painter.pen().brush());
                brush.resetTransform();
                brush.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                brush.drawImage(QPoint(), tinted(mask, Qt::white));
            }
            painter.save();
            painter.setWorldTransform(transform.inverted() * painter.worldTransform());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(tile.topLeft(), pixels);
            painter.restore();
        }
    }
    ++s_rasterDraws;
    return true;
}

bool paintRaster(QPainter& painter, const QPointF& baseline, const QString& text)
{
    const auto transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0
        || !qFuzzyCompare(transform.m11(), transform.m22())
        || painter.pen().brush().style() != Qt::SolidPattern)
        return paintTransformed(painter, baseline, text);
    QFont font = nativeFont(painter);
    QPoint offset;
    const QImage mask = coverage(font, text, &offset);
    if (mask.isNull())
        return paintTransformed(painter, baseline, text);
    const QPointF deviceBaseline = transform.map(baseline);
    const QPoint origin(qRound(deviceBaseline.x()) + offset.x(), qRound(deviceBaseline.y()) + offset.y());
    const QColor ink = painter.pen().color();
    const QString key = font.key() + QChar('|') + QString::number(ink.rgba(), 16) + QChar('|') + text;
    static thread_local QCache<QString, QImage> colored(4 * 1024);
    QImage* image = colored.object(key);
    std::unique_ptr<QImage> oversized;
    if (!image) {
        image = new QImage(tinted(mask, ink));
        const int cost = qMax(1, int((image->sizeInBytes() + key.size() * sizeof(QChar) + 1023) / 1024));
        if (cost > colored.maxCost()) oversized.reset(image);
        else colored.insert(key, image, cost);
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRectF(transform.inverted().map(QPointF(origin)), QSizeF(image->size()) / transform.m11()), *image);
    painter.restore();
    ++s_rasterDraws;
    return true;
}
}

void setEnabled(bool enabled) { s_enabled = enabled; }
bool enabled() { return s_enabled; }

QImage coverage(const QFont& font, const QString& text, QPoint* offset)
{
    struct Glyph { QImage mask; QPoint offset; };
    static thread_local QCache<QString, Glyph> cache(8 * 1024);
    QFont unified(font);
    unified.setFamily(AppFonts::family());
    unified.setHintingPreference(QFont::PreferNoHinting);
    const QString key = unified.key() + QChar('|') + text;
    if (const auto* glyph = cache.object(key)) {
        if (offset) *offset = glyph->offset;
        return glyph->mask;
    }
    const QPainterPath outline = textOutline(unified, text);
    const QRect bounds = outline.boundingRect().toAlignedRect().adjusted(-2,-2,2,2);
    if (offset)
        *offset = bounds.topLeft();
    if (bounds.isEmpty() || qint64(bounds.width()) * bounds.height() > 1024 * 1024)
        return {};
    QImage result = rasterizeOutline(outline, bounds, unified.styleStrategy() & QFont::NoAntialias);
    const int cost = qMax(1, int((result.sizeInBytes() + key.size() * sizeof(QChar) + 1023) / 1024));
    if (cost <= cache.maxCost())
        cache.insert(key, new Glyph{result, bounds.topLeft()}, cost);
    return result;
}

bool draw(QPainter& painter, const QPointF& baseline, const QString& text)
{
    if (text.isEmpty() || painter.pen().style() == Qt::NoPen) return true;
    if (auto* device = AppText::PaintDevice::forPainter(painter))
        return device->drawText(painter, baseline, text);
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
        || painter.pen().brush().style() != Qt::SolidPattern
        || painter.compositionMode() != QPainter::CompositionMode_SourceOver
        || (painter.hasClipping() && painter.clipPath().elementCount() > 5))
        return false;
    const auto transform = painter.deviceTransform();
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0
        || !qFuzzyCompare(transform.m11(), transform.m22()))
        return false;
    const QFont font = painter.font();
    const QFont rasterFont = nativeFont(painter);
    const QString key = rasterFont.key() + QChar('|') + text;
    Mask* mask = m_masks.object(key);
    auto* gl = QOpenGLContext::currentContext()->extraFunctions();
    if (!mask) {
        auto created = std::make_unique<Mask>();
        created->pixels = coverage(rasterFont, text, &created->offset);
        if (created->pixels.isNull())
            return false;
        created->size = created->pixels.size();
        // 先保留 CPU 掩码，纹理在 QPainter 切回原生绘制之后上传。
        const int cost = qMax(1, int((created->pixels.sizeInBytes() + key.size() * sizeof(QChar) + 1023) / 1024));
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
            void main() {
                float alpha = texture2D(maskTexture, uv).r * ink.a;
                gl_FragColor = vec4(ink.rgb * alpha, alpha);
            }
        )";
        m_shader->bindAttributeLocation("position", 0);
        if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !m_shader->link()) {
            RenderDiagnostics::write("text.grayscale.failed", m_shader->log(), 10000);
            m_shader.reset();
            painter.endNativePainting();
            return false;
        }
        // 链接后缓存位置，避免每个字串和每个颜色通道重复查询驱动。
        m_viewportLocation = m_shader->uniformLocation("viewport");
        m_rectangleLocation = m_shader->uniformLocation("rectangle");
        m_maskTextureLocation = m_shader->uniformLocation("maskTexture");
        m_inkLocation = m_shader->uniformLocation("ink");
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
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, mask->pixels.width(), mask->pixels.height(),
                         0, GL_LUMINANCE, GL_UNSIGNED_BYTE, mask->pixels.constBits());
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
    // Grayscale coverage uses one premultiplied source-over pass.
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisableVertexAttribArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_shader->release();
    painter.endNativePainting();
    return true;
}
}
