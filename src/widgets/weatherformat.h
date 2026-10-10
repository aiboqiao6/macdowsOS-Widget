#pragma once
#include <QString>

// 网络解析与卡片绘制共用的天气文本规范化。
namespace WeatherFormat {
inline QString weatherTemperatureLabel(QString value)
{
    value = value.trimmed();
    value.remove(QChar(0x2103));
    value.remove(QChar(0x00B0));
    value = value.trimmed();
    return (value.isEmpty() ? QStringLiteral("--") : value) + QChar(0x00B0);
}

inline QString normalizedWeekdayLabel(QString value)
{
    value = value.trimmed();
    if (!value.startsWith(QStringLiteral("星期")))
        return value;

    QString suffix = value.mid(2);
    if (suffix == QStringLiteral("天"))
        suffix = QStringLiteral("日");
    return suffix.isEmpty() ? value : QStringLiteral("周") + suffix;
}
}
