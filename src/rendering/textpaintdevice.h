#pragma once

#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <memory>

// Qt owns layout, selection and input. This adapter routes the resulting text
// operations through our outline scanner, while forwarding all other painting.
namespace AppText {
class PaintDevice final : public QPaintDevice
{
public:
    explicit PaintDevice(QPainter& destination);
    ~PaintDevice() override;
    QPaintEngine* paintEngine() const override;
    bool drawText(QPainter& source, const QPointF& baseline, const QString& text);
    static PaintDevice* forPainter(const QPainter& painter);
protected:
    int metric(PaintDeviceMetric metric) const override;
private:
    class Engine;
    QPainter& m_destination;
    std::unique_ptr<Engine> m_engine;
};
}
