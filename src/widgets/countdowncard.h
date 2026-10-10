#pragma once

#include <QDate>
#include <QRectF>
#include <QString>
class QPainter;

namespace CountdownCard {
struct Event {
    QString title = QStringLiteral("值得期待的一天");
    QDate date = QDate::currentDate().addDays(30);
    bool annual = false;
};
struct State {
    QDate occurrence;
    qint64 days = 0;
    bool elapsed = false;
};
// 使用本地日历日计算，避免夏令时和跨午夜的毫秒差造成天数偏移。
State state(const Event& event, const QDate& today = QDate::currentDate());
void paint(QPainter& painter, const QRectF& bounds, const Event& event,
           const QDate& today = QDate::currentDate());
}
