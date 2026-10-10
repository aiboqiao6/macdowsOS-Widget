#include "widgets/desktopwidget.h"
#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"
#include <QPainter>
#include <QtMath>

using namespace CardMetrics;
using namespace WidgetGeometry;

QImage DesktopWidget::renderPreview(CardKind kind, int variant, const QSize& size,
                                    qreal devicePixelRatio)
{
    if (size.isEmpty())
        return {};
    DesktopWidget preview(nullptr, kind, false,
                          QStringLiteral("gallery-preview"), variant, true);
    // 保留卡片真实网格比例，不强行拉伸到图库统一矩形。
    const QSize reference = gridWidgetSize(kind, variant, kGridBaseCell,
                                           qRound(kGridBaseGap));
    QSize renderSize = reference;
    renderSize.scale(size, Qt::KeepAspectRatio);
    renderSize = renderSize.expandedTo(QSize(1, 1));
    preview.m_widgetScale = qMin(renderSize.width() / qreal(reference.width()),
                             renderSize.height() / qreal(reference.height()));
    preview.setMinimumSize(0, 0);
    preview.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    preview.resize(renderSize);

    // 未显示的 GL 窗口可能没有帧缓冲；直接向图像调用相同内容绘制，避免空白预览。
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatio);
    QImage image(QSize(qMax(1, qRound(renderSize.width() * dpr)),
                      qMax(1, qRound(renderSize.height() * dpr))),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    // 预览仅绘制内容，透明区域复用图库的统一玻璃，不叠加独立色调。
    preview.paintOverlay(painter);
    painter.end();
    return image;
}

