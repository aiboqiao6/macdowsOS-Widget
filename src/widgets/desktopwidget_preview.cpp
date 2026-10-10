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
    // 图库展示离线示例内容，不让未启动的数据服务留下加载占位。
    preview.m_weatherLocation = QStringLiteral("北京");
    preview.m_weatherTemperature = QStringLiteral("21");
    preview.m_weatherDescription = QStringLiteral("晴朗无云");
    preview.m_weatherHigh = QStringLiteral("26");
    preview.m_weatherLow = QStringLiteral("19");
    preview.m_weatherCode = 0;
    for (int i = 0; i < 6; ++i)
        preview.m_weatherHours.append({QStringLiteral("%1时").arg(10 + i),
                                       QString::number(21 + i % 3), 0, false});
    for (int i = 0; i < 5; ++i)
        preview.m_weatherDays.append({QStringLiteral("周%1").arg(i + 1),
                                      QString::number(26 + i % 3), QStringLiteral("19"), 0, false});
    preview.m_dictionaryWord = QStringLiteral("serendipity");
    preview.m_dictionaryPhonetic = QStringLiteral("/ˌserənˈdɪpəti/");
    preview.m_dictionaryPartOfSpeech = QStringLiteral("n. 名词");
    preview.m_dictionaryDefinition = QStringLiteral("不期而遇的美好；意外发现的幸运。");
    preview.m_hasBattery = true;
    preview.m_batteryLevel = 86;
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
    // 缩略图使用统一的深色展示画布，保留独立卡片轮廓，避免文字直接浮在图库背景上。
    const QRectF card{QPointF(), QSizeF(renderSize)};
    const qreal radius = LiquidGlassStyle::kCardCornerRadius * preview.m_widgetScale;
    painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0 / dpr));
    painter.setBrush(preview.darkModeEnabled() ? QColor(3, 16, 19, 250)
                                             : QColor(18, 35, 40, 245));
    painter.drawRoundedRect(card.adjusted(.5 / dpr, .5 / dpr, -.5 / dpr, -.5 / dpr),
                            radius, radius);
    preview.paintOverlay(painter);
    painter.end();
    return image;
}

