#pragma once
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace CardRequest {
void configureWeather(QNetworkRequest& request);
// 返回带传输超时和总时限的回复，调用方负责标识、解析与释放。
QNetworkReply* get(QNetworkAccessManager& network, QNetworkRequest request);
}
