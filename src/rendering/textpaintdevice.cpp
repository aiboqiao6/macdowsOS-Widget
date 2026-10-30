#include "rendering/textpaintdevice.h"
#include "rendering/cleartypetext.h"

#include <QPainterPath>

namespace AppText {
class PaintDevice::Engine final : public QPaintEngine
{
public:
    explicit Engine(QPainter& destination)
        : QPaintEngine(AllFeatures), m_painter(destination) {}
    bool begin(QPaintDevice* device) override
    {
        setPaintDevice(device);
        m_painter.save();
        m_transform = m_painter.worldTransform();
        m_opacity = m_painter.opacity();
        m_hasBaseClip = m_painter.hasClipping();
        if (m_hasBaseClip) m_baseClip = m_painter.clipPath();
        return true;
    }
    bool end() override { m_painter.restore(); return true; }
    Type type() const override { return User; }
    bool drawCustomText(QPainter& source, const QPointF& baseline, const QString& text)
    {
        // A QPainter save() can leave engine.state pointing at an earlier state
        // until the next native operation. Read the current public painter state
        // directly, and restore our destination so native state stays in sync.
        m_painter.save();
        m_painter.setWorldTransform(source.worldTransform() * m_transform);
        m_painter.setFont(source.font());
        m_painter.setPen(source.pen());
        m_painter.setBrushOrigin(source.brushOrigin());
        m_painter.setOpacity(m_opacity * source.opacity());
        m_painter.setCompositionMode(source.compositionMode());
        m_painter.setClipping(false);
        if (m_hasBaseClip) {
            const QTransform current = m_painter.worldTransform();
            m_painter.setWorldTransform(m_transform);
            m_painter.setClipPath(m_baseClip);
            m_painter.setWorldTransform(current);
        }
        if (source.hasClipping()) m_painter.setClipPath(source.clipPath(), Qt::IntersectClip);
        const bool result = ClearTypeText::draw(m_painter, baseline, text);
        m_painter.restore();
        return result;
    }
    void updateState(const QPaintEngineState& state) override
    {
        const auto dirty = state.state();
        if (dirty & DirtyTransform)
            m_painter.setWorldTransform(state.transform() * m_transform);
        if (dirty & DirtyPen) m_painter.setPen(state.pen());
        if (dirty & DirtyBrush) m_painter.setBrush(state.brush());
        if (dirty & DirtyBrushOrigin) m_painter.setBrushOrigin(state.brushOrigin());
        if (dirty & DirtyFont) m_painter.setFont(state.font());
        if (dirty & DirtyBackground) m_painter.setBackground(state.backgroundBrush());
        if (dirty & DirtyBackgroundMode) m_painter.setBackgroundMode(state.backgroundMode());
        if (dirty & DirtyOpacity) m_painter.setOpacity(m_opacity * state.opacity());
        if (dirty & DirtyCompositionMode) m_painter.setCompositionMode(state.compositionMode());
        if (dirty & DirtyHints) {
            m_painter.setRenderHints(m_painter.renderHints(), false);
            m_painter.setRenderHints(state.renderHints());
        }
        if (dirty & (DirtyClipEnabled | DirtyClipPath | DirtyClipRegion)) {
            // Rebuild the complete clip on restore as well as on setClipRect.
            // The destination's system clip remains in force throughout.
            m_painter.setClipping(false);
            if (m_hasBaseClip) {
                const QTransform current = m_painter.worldTransform();
                m_painter.setWorldTransform(m_transform);
                m_painter.setClipPath(m_baseClip, Qt::ReplaceClip);
                m_painter.setWorldTransform(current);
            }
            if (state.isClipEnabled())
                m_painter.setClipPath(state.painter()->clipPath(), Qt::IntersectClip);
        }
    }
    void drawTextItem(const QPointF& baseline, const QTextItem& item) override
    {
        m_painter.save();
        QFont font = item.font();
        font.setUnderline(font.underline() || (item.renderFlags() & QTextItem::Underline));
        font.setOverline(font.overline() || (item.renderFlags() & QTextItem::Overline));
        font.setStrikeOut(font.strikeOut() || (item.renderFlags() & QTextItem::StrikeOut));
        m_painter.setFont(font);
        ClearTypeText::draw(m_painter, baseline, item.text());
        m_painter.restore();
    }
    void drawPixmap(const QRectF& r, const QPixmap& p, const QRectF& source) override
    { m_painter.drawPixmap(r, p, source); }
    void drawImage(const QRectF& r, const QImage& image, const QRectF& source,
                   Qt::ImageConversionFlags flags) override
    { m_painter.drawImage(r, image, source, flags); }
    void drawTiledPixmap(const QRectF& r, const QPixmap& p, const QPointF& origin) override
    { m_painter.drawTiledPixmap(r, p, origin); }
    void drawPath(const QPainterPath& path) override { m_painter.drawPath(path); }
    void drawRects(const QRectF* r, int n) override { m_painter.drawRects(r, n); }
    void drawRects(const QRect* r, int n) override { m_painter.drawRects(r, n); }
    void drawLines(const QLineF* lines, int n) override { m_painter.drawLines(lines, n); }
    void drawLines(const QLine* lines, int n) override { m_painter.drawLines(lines, n); }
    void drawEllipse(const QRectF& r) override { m_painter.drawEllipse(r); }
    void drawEllipse(const QRect& r) override { m_painter.drawEllipse(r); }
    void drawPoints(const QPointF* points, int n) override { m_painter.drawPoints(points, n); }
    void drawPoints(const QPoint* points, int n) override { m_painter.drawPoints(points, n); }
    void drawPolygon(const QPointF* points, int n, PolygonDrawMode mode) override
    {
        if (mode == PolylineMode) m_painter.drawPolyline(points, n);
        else m_painter.drawPolygon(points, n, mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
    }
    void drawPolygon(const QPoint* points, int n, PolygonDrawMode mode) override
    {
        if (mode == PolylineMode) m_painter.drawPolyline(points, n);
        else m_painter.drawPolygon(points, n, mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
    }
private:
    QPainter& m_painter;
    QTransform m_transform;
    qreal m_opacity = 1;
    bool m_hasBaseClip = false;
    QPainterPath m_baseClip;
};

PaintDevice::PaintDevice(QPainter& destination)
    : m_destination(destination), m_engine(std::make_unique<Engine>(destination)) {}
PaintDevice::~PaintDevice() = default;
QPaintEngine* PaintDevice::paintEngine() const { return m_engine.get(); }

PaintDevice* PaintDevice::forPainter(const QPainter& painter)
{
    // For redirected widgets QPainter::device() returns the original widget.
    // The engine's device identifies this adapter in both direct and widget use.
    const auto* engine = dynamic_cast<Engine*>(painter.paintEngine());
    return engine ? static_cast<PaintDevice*>(engine->paintDevice()) : nullptr;
}

bool PaintDevice::drawText(QPainter& source, const QPointF& baseline, const QString& text)
{
    return m_engine->drawCustomText(source, baseline, text);
}

int PaintDevice::metric(PaintDeviceMetric metric) const
{
    const auto* target = m_destination.device();
    switch (metric) {
    case PdmWidth: return target->width();
    case PdmHeight: return target->height();
    case PdmWidthMM: return target->widthMM();
    case PdmHeightMM: return target->heightMM();
    case PdmDpiX: return target->logicalDpiX();
    case PdmDpiY: return target->logicalDpiY();
    case PdmPhysicalDpiX: return target->physicalDpiX();
    case PdmPhysicalDpiY: return target->physicalDpiY();
    case PdmDepth: return target->depth();
    case PdmNumColors: return 0;
    // The destination already applies DPR. Applying it here would scale twice.
    case PdmDevicePixelRatio: return 1;
    case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
    case PdmDevicePixelRatioF_EncodedA:
    case PdmDevicePixelRatioF_EncodedB: return encodeMetricF(metric, 1.0);
    default: return QPaintDevice::metric(metric);
    }
}
}
