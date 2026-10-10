#include "network/cardrequest.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>

namespace CardRequest {
void configureWeather(QNetworkRequest& request)
{
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
    request.setRawHeader("Referer", "https://www.weather.com.cn/");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
}

QNetworkReply* get(QNetworkAccessManager& network, QNetworkRequest request)
{
    request.setTransferTimeout(15000);
    QNetworkReply* reply = network.get(request);
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    return reply;
}
}
