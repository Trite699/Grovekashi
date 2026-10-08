//////////////////////////////////////////////////////////////////////////////////////
//    akashi - a server for Attorney Online 2                                       //
//    Copyright (C) 2020  scatterflower                                             //
//                                                                                  //
//    This program is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU Affero General Public License as                //
//    published by the Free Software Foundation, either version 3 of the            //
//    License, or (at your option) any later version.                               //
//                                                                                  //
//    This program is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of                //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                 //
//    GNU Affero General Public License for more details.                           //
//                                                                                  //
//    You should have received a copy of the GNU Affero General Public License      //
//    along with this program.  If not, see <https://www.gnu.org/licenses/>.        //
//////////////////////////////////////////////////////////////////////////////////////
#include "serverpublisher.h"
#include "config_manager.h"
#include "qnamespace.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

const int HTTP_OK = 200;
const int WS_REVERSE_PROXY = 80;
const int TIMEOUT = 1000 * 60 * 4;
const int LEGACY_TCP_PORT = 27106;

ServerPublisher::ServerPublisher(int port, int *player_count, QObject *parent) :
    QObject(parent),
    m_manager{new QNetworkAccessManager(this)},
    timeout_timer(new QTimer(this)),
    m_players(player_count),
    m_port{port}
{
    connect(m_manager, &QNetworkAccessManager::finished, this, &ServerPublisher::finished);
    connect(timeout_timer, &QTimer::timeout, this, &ServerPublisher::publishServer);

    timeout_timer->setTimerType(Qt::PreciseTimer);
    timeout_timer->setInterval(TIMEOUT);
    timeout_timer->start();
    publishServer();
}

void ServerPublisher::publishServer()
{
    if (!ConfigManager::publishServerEnabled()) {
        return;
    }

    const QList<QUrl> serverlists = ConfigManager::serverlistURLs();
    if (serverlists.isEmpty()) {
        qWarning() << "Failed to advertise server. No serverlist URL configured.";
        return;
    }

    QJsonObject serverinfo;
    const QString hostname = ConfigManager::serverDomainName().trimmed();
    if (!hostname.isEmpty()) {
        serverinfo["ip"] = hostname;
    }
    const int secure_port = ConfigManager::securePort();
    if (secure_port > 0 && secure_port <= 65535) {
        serverinfo["wss_port"] = secure_port;
    }
    // The legacy TCP port is no longer used, but the masterserver still requires the field.
    serverinfo["port"] = LEGACY_TCP_PORT;
    const int ws_port = ConfigManager::advertiseWSProxy() ? WS_REVERSE_PROXY : m_port;
    if (ws_port <= 0 || ws_port > 65535) {
        qWarning() << "Failed to advertise server. Invalid websocket port:" << ws_port;
        return;
    }
    serverinfo["ws_port"] = ws_port;
    serverinfo["players"] = *m_players;
    // Masterservers reject an empty name with "400 Bad Request".
    QString name = ConfigManager::serverName().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("An Unnamed Server");
    }
    serverinfo["name"] = name;
    serverinfo["description"] = ConfigManager::serverDescription().trimmed();
    const QByteArray payload = QJsonDocument(serverinfo).toJson(QJsonDocument::Compact);

    // Each ms gets its own independent POST for avoiding fucking shit up.
    for (const QUrl &serverlist : serverlists) {
        if (!serverlist.isValid()) {
            qWarning() << "Failed to advertise server. Serverlist URL is not valid. URL:" << serverlist.toString();
            continue;
        }
        QNetworkRequest request(serverlist);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json");
        // Apply the HTTP2 setting here where the request is actually being sent
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        m_manager->post(request, payload);
    }
}

void ServerPublisher::finished(QNetworkReply *f_reply)
{
    QNetworkReply *reply(f_reply);
    reply->deleteLater();
    const QString remote_url = reply->url().toString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Always read the body, even on HTTP errors. The masterserver explains
    // what it didn't like (e.g. a 400) in the response body.
    const QByteArray data = reply->isReadable() ? reply->readAll() : QByteArray();

    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        qWarning() << "Unable to advertise to serverlist:" << reply->errorString();
        qWarning() << "Remote URL:" << remote_url;
        if (status != 0) {
            qWarning() << "HTTP status code:" << status;
        }
        logResponseErrors(data);
        return;
    }

    if (status != HTTP_OK) {
        logResponseErrors(data);
        return;
    }
    qInfo() << "Successfully advertised server to serverlist:" << remote_url;
}

void ServerPublisher::logResponseErrors(const QByteArray &f_data)
{
    if (f_data.trimmed().isEmpty()) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(f_data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning().noquote() << "Masterserver response body:" << QString::fromUtf8(f_data.left(1024));
        return;
    }

    const QJsonObject body = document.object();
    if (body.contains("errors")) {
        const QJsonArray errors = body["errors"].toArray();
        for (const auto &ref : errors) {
            const QJsonObject err = ref.toObject();
            qWarning().noquote() << "Masterserver error:" << err["type"].toString() << "-" << err["message"].toString();
        }
    }
    else {
        qWarning().noquote() << "Masterserver response body:" << QString::fromUtf8(f_data.left(1024));
    }
}
