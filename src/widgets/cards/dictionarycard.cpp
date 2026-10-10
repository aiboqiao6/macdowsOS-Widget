#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QFontMetrics>

using namespace CardMetrics;
using AppFonts::displayFont;

void DesktopWidget::paintDictionaryCard(QPainter& painter, const QRectF& card) const
{
    painter.save();
    const ResponsiveLayout::Metrics dictionaryMetrics(card);
    if (m_cardVariant == 0 || m_cardVariant == 2) {
        const QRectF content = card.adjusted(28, 26, -28, -24);
        painter.setFont(displayFont(m_cardVariant == 0 ? 26 : 30, QFont::DemiBold));
        painter.setPen(QColor(247, 250, 255, 238));
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top(), content.width(), 42),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…")
                                       : m_dictionaryWord);
        painter.setFont(displayFont(13, QFont::Normal));
        painter.setPen(QColor(191, 203, 224, 205));
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.top() + 52, content.width(), 24),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryPhonetic.isEmpty() ? QStringLiteral("每日单词")
                                           : m_dictionaryPhonetic);
        painter.setFont(displayFont(11, QFont::Normal));
        painter.setPen(QColor(170, 184, 211, 185));
        ResponsiveLayout::drawSingleLine(
            painter, QRectF(content.left(), content.bottom() - 30,
                      content.width(), 20),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_cardVariant == 0 ? QStringLiteral("点击刷新")
                           : QStringLiteral("在线词典"));
        painter.restore();
        return;
    }
    const QRectF content = card.adjusted(32, 26, -30, -24);
    painter.setFont(displayFont(34, QFont::DemiBold));
    painter.setPen(QColor(244, 247, 254, 232));
    ResponsiveLayout::drawSingleLine(
        painter, QRectF(content.left(), content.top(), content.width(), 42),
        Qt::AlignLeft | Qt::AlignVCenter,
        m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…") : m_dictionaryWord);
    painter.setFont(displayFont(18, QFont::Normal));
    painter.setPen(QColor(218, 226, 242, 220));
    const QString phonetic = m_dictionaryPhonetic.isEmpty()
                                 ? QStringLiteral("词典数据") : m_dictionaryPhonetic;
    ResponsiveLayout::drawSingleLine(
        painter, QRectF(content.left(), content.top() + 46, content.width(), 28),
        Qt::AlignLeft | Qt::AlignVCenter, phonetic);
    painter.setFont(displayFont(16, QFont::Normal));
    ResponsiveLayout::drawSingleLine(
        painter, QRectF(content.left(), content.top() + 82, content.width(), 28),
        Qt::AlignLeft | Qt::AlignVCenter,
        m_dictionaryPartOfSpeech.isEmpty() ? QStringLiteral("在线词典")
                                           : m_dictionaryPartOfSpeech);
    ResponsiveLayout::drawSingleLine(
        painter, QRectF(content.left(), content.top() + 118, content.width(), 42),
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(painter.font()).elidedText(m_dictionaryDefinition,
                                          Qt::ElideRight, int(content.width())));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(198, 210, 238, 105));
    const qreal barWidth = qMin<qreal>(content.width() - 110, 560.0);
    for (int i = 0; i < 20; ++i)
        painter.drawRoundedRect(QRectF(content.left() + i * (barWidth / 20.0),
                                 content.top() + 164, barWidth / 20.0 - 4, 20), 4, 4);
    painter.setPen(QPen(QColor(215, 226, 250, 180),
                  dictionaryMetrics.stroke(painter, .003)));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(content.left(), card.bottom() - 52, 82, 32), 6, 6);
    painter.setFont(displayFont(14, QFont::Normal));
    ResponsiveLayout::drawSingleLine(painter, QRectF(content.left(), card.bottom() - 52,
                                         82, 32),
                               Qt::AlignCenter, QStringLiteral("随机单词"));
    painter.restore();
}
