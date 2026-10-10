#pragma once
#include <QString>
class QWidget;
class QNetworkAccessManager;

namespace WeatherCityDialog {
struct Selection {
    QString cityId;
    QString name;
};
// 对话框只返回选择结果，配置和卡片刷新交由调用方处理。
Selection choose(QWidget* parent, QNetworkAccessManager* network, const QString& currentLocation);
}
