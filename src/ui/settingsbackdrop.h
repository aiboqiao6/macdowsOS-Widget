#pragma once

#include "settingsappearance.h"
#include <QImage>
#include <QFutureWatcher>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Settings uses a plain separable blur, independent of the widget glass material.
// No refraction, noise, lighting or shader rim is applied to these pixels.
class SettingsBackdrop final : public QWidget
{
public:
    explicit SettingsBackdrop(QWidget* parent) : QWidget(parent), m_worker(this), m_releaseBuffers(this)
    {
        m_releaseBuffers.setSingleShot(true);
        m_releaseBuffers.setInterval(250);
        connect(&m_releaseBuffers, &QTimer::timeout, this, [this] {
            if (!m_processing && m_pending.isNull()) m_buffers.reset();
        });
        connect(&m_worker, &QFutureWatcher<QImage>::finished, this, [this] {
            QImage image = m_worker.result();
            m_processing = false;
            if (m_workEpoch == m_epoch && !image.isNull()) {
                m_image = std::move(image);
                update();
                if (presented) presented();
            }
            startPending();
            if (!m_processing && m_buffers) m_releaseBuffers.start();
        });
    }

    void setCornerRadius(qreal radius) { if (m_corner != radius) { m_corner = radius; update(); } }
    QSize backdropPixelSize() const { return m_image.size(); }
    bool hasPendingBackdrop() const { return !m_pending.isNull(); }
    std::function<void()> presented;

    void setBackdrop(const QImage& source, qreal radius)
    {
        if (source.isNull()) return;
        const qreal scale = source.width() / qreal(qMax(1, width()));
        m_pending = source;
        m_pendingRadius = qMax(1, qRound(radius * scale));
        startPending();
    }

    void clearPending()
    {
        ++m_epoch;
        m_pending = {};
        m_image = {};
        m_releaseBuffers.stop();
        m_buffers.reset();
    }

    // The same six-pass integer blur is used by the worker and pixel regression.
    static QImage blurred(const QImage& source, int radius)
    {
        BlurBuffers buffers;
        return blurred(source, radius, buffers);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.setFillRule(Qt::WindingFill);
        clip.addRoundedRect(QRectF(rect()), m_corner, m_corner);
        // The inner edge meets the opaque content panel with square corners.
        clip.addRect(QRectF(width() - m_corner, 0, m_corner, height()));
        painter.setClipPath(clip);
        painter.drawImage(rect(), m_image);
    }

private:
    struct Sum { int r = 0, g = 0, b = 0; };
    struct BlurBuffers {
        QImage scratch;
        std::vector<Sum> sums;
        std::vector<uchar> normalized;
        int denominator = 0;
    };

    static QImage blurred(const QImage& source, int radius, BlurBuffers& buffers)
    {
        if (source.isNull()) return {};
        QImage pixels = source.convertToFormat(QImage::Format_RGB32);
        pixels.detach();
        if (buffers.scratch.size() != pixels.size())
            buffers.scratch = QImage(pixels.size(), QImage::Format_RGB32);
        buffers.sums.resize(pixels.width());
        radius = qMax(1, radius);
        const int count = radius * 2 + 1;
        // A bounded byte table replaces three integer divides per pixel while
        // preserving every rounded result. Large custom radii use division.
        if (buffers.denominator != count) {
            buffers.normalized.resize(count <= 257 ? count * 255 + 1 : 0);
            for (size_t sum = 0; sum < buffers.normalized.size(); ++sum)
                buffers.normalized[sum] = uchar(sum / count);
            buffers.denominator = count;
        }
        const uchar* normalized = buffers.normalized.empty() ? nullptr : buffers.normalized.data();
        for (int pass = 0; pass < 3; ++pass) {
            blurHorizontal(pixels, buffers.scratch, radius, normalized);
            blurVertical(buffers.scratch, pixels, radius, normalized, buffers.sums);
        }
        return pixels;
    }

    void startPending()
    {
        if (m_processing || m_pending.isNull()) return;
        m_releaseBuffers.stop();
        m_processing = true;
        m_workEpoch = m_epoch;
        QImage source = std::exchange(m_pending, {});
        const int radius = m_pendingRadius;
        if (!m_buffers) m_buffers = std::make_shared<BlurBuffers>();
        m_worker.setFuture(QtConcurrent::run([source = std::move(source), radius, buffers = m_buffers] {
            return blurred(source, radius, *buffers);
        }));
    }

    static void blurHorizontal(const QImage& input, QImage& output, int radius, const uchar* normalized)
    {
        const int width = input.width(), height = input.height();
        const int count = radius * 2 + 1;
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(input.constScanLine(y));
            auto* destination = reinterpret_cast<QRgb*>(output.scanLine(y));
            int red = 0, green = 0, blue = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const QRgb c = row[qBound(0, offset, width - 1)];
                red += qRed(c); green += qGreen(c); blue += qBlue(c);
            }
            for (int x = 0; x < width; ++x) {
                destination[x] = normalized ? qRgb(normalized[red], normalized[green], normalized[blue])
                    : qRgb(red / count, green / count, blue / count);
                const QRgb before = row[qMax(0, x - radius)], after = row[qMin(width - 1, x + radius + 1)];
                red += qRed(after) - qRed(before);
                green += qGreen(after) - qGreen(before);
                blue += qBlue(after) - qBlue(before);
            }
        }
    }

    static void blurVertical(const QImage& input, QImage& output, int radius,
                             const uchar* normalized, std::vector<Sum>& sums)
    {
        const int width = input.width(), height = input.height();
        const int count = radius * 2 + 1;
        std::fill(sums.begin(), sums.end(), Sum{});
        for (int offset = -radius; offset <= radius; ++offset) {
            const auto* row = reinterpret_cast<const QRgb*>(input.constScanLine(qBound(0, offset, height - 1)));
            for (int x = 0; x < width; ++x) {
                sums[x].r += qRed(row[x]); sums[x].g += qGreen(row[x]); sums[x].b += qBlue(row[x]);
            }
        }
        for (int y = 0; y < height; ++y) {
            auto* destination = reinterpret_cast<QRgb*>(output.scanLine(y));
            const auto* before = reinterpret_cast<const QRgb*>(input.constScanLine(qMax(0, y - radius)));
            const auto* after = reinterpret_cast<const QRgb*>(input.constScanLine(qMin(height - 1, y + radius + 1)));
            for (int x = 0; x < width; ++x) {
                auto& sum = sums[x];
                destination[x] = normalized ? qRgb(normalized[sum.r], normalized[sum.g], normalized[sum.b])
                    : qRgb(sum.r / count, sum.g / count, sum.b / count);
                sum.r += qRed(after[x]) - qRed(before[x]);
                sum.g += qGreen(after[x]) - qGreen(before[x]);
                sum.b += qBlue(after[x]) - qBlue(before[x]);
            }
        }
    }

    QImage m_image;
    QImage m_pending;
    QFutureWatcher<QImage> m_worker;
    QTimer m_releaseBuffers;
    std::shared_ptr<BlurBuffers> m_buffers;
    quint64 m_epoch = 0, m_workEpoch = 0;
    int m_pendingRadius = 1;
    bool m_processing = false;
    qreal m_corner = 0;
};
