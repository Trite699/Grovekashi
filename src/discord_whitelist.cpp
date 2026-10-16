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
#include "discord_whitelist.h"

#include "aoclient.h"
#include "config_manager.h"
#include "server.h"

#include <QDateTime>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace {
// Discord gateway close codes after which reconnecting can never work.
bool isFatalCloseCode(int f_code)
{
    return f_code == 4004 || f_code == 4010 || f_code == 4011 || f_code == 4012 || f_code == 4013 || f_code == 4014;
}

QString describeCloseCode(int f_code)
{
    switch (f_code) {
    case 4004:
        return "The bot token is wrong.";
    case 4013:
    case 4014:
        return "The bot is not allowed to use the \"Server Members Intent\". Switch it on in the Discord developer portal under Bot -> Privileged Gateway Intents.";
    default:
        return "Discord closed the connection with code " + QString::number(f_code) + ".";
    }
}
} // namespace

DiscordWhitelist::DiscordWhitelist(Server *f_server, QObject *parent) :
    QObject(parent),
    m_server(f_server),
    m_socket("akashi", QWebSocketProtocol::VersionLatest, this)
{
    m_token = ConfigManager::whitelistToken();
    m_channel_id = ConfigManager::whitelistChannel();
    m_api_url = ConfigManager::whitelistApiUrl();
    m_gateway_url = ConfigManager::whitelistGatewayUrl();

    m_heartbeat_timer.setParent(this);
    m_reconnect_timer.setParent(this);
    m_reconnect_timer.setSingleShot(true);

    connect(&m_socket, &QWebSocket::connected, this, &DiscordWhitelist::onGatewayConnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &DiscordWhitelist::onGatewayMessage);
    connect(&m_socket, &QWebSocket::disconnected, this, &DiscordWhitelist::onGatewayDisconnected);
    connect(&m_heartbeat_timer, &QTimer::timeout, this, &DiscordWhitelist::onHeartbeatTimer);
    connect(&m_reconnect_timer, &QTimer::timeout, this, &DiscordWhitelist::connectGateway);
}

void DiscordWhitelist::start()
{
    connectGateway();
}

bool DiscordWhitelist::isConnected() const
{
    return m_ready;
}

// ---------------------------------------------------------------------------------------------------------------
// REST
// ---------------------------------------------------------------------------------------------------------------

void DiscordWhitelist::api(const QByteArray &f_verb, const QString &f_path, const QJsonObject &f_body, ApiCallback f_callback)
{
    QNetworkRequest l_request(QUrl(m_api_url + f_path));
    l_request.setRawHeader("Authorization", "Bot " + m_token.toUtf8());
    l_request.setRawHeader("User-Agent", "DiscordBot (https://github.com/AttorneyOnline/akashi, 1.0)");
    l_request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QByteArray l_payload = f_body.isEmpty() ? QByteArray() : QJsonDocument(f_body).toJson(QJsonDocument::Compact);
    QNetworkReply *l_reply = m_network.sendCustomRequest(l_request, f_verb, l_payload);
    connect(l_reply, &QNetworkReply::finished, this, [l_reply, f_callback]() {
        const int l_status = l_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray l_data = l_reply->readAll();
        l_reply->deleteLater();
        if (f_callback) {
            f_callback(l_status, QJsonDocument::fromJson(l_data));
        }
    });
}

void DiscordWhitelist::ensureGuild(std::function<void(bool f_ok)> f_callback)
{
    if (!m_guild_id.isEmpty()) {
        f_callback(true);
        return;
    }
    api("GET", "/channels/" + m_channel_id, QJsonObject(), [this, f_callback](int f_status, const QJsonDocument &f_body) {
        if (f_status != 200 || f_body.object().value("guild_id").toString().isEmpty()) {
            qWarning() << "Whitelist: could not find the whitelist channel. HTTP status:" << f_status;
            f_callback(false);
            return;
        }
        m_guild_id = f_body.object().value("guild_id").toString();
        f_callback(true);
    });
}

void DiscordWhitelist::findMember(const QString &f_discord_name, std::function<void(const QString &f_member_id)> f_callback)
{
    QUrlQuery l_query;
    l_query.addQueryItem("query", f_discord_name);
    l_query.addQueryItem("limit", "100");
    api("GET", "/guilds/" + m_guild_id + "/members/search?" + l_query.toString(QUrl::FullyEncoded), QJsonObject(), [f_discord_name, f_callback](int f_status, const QJsonDocument &f_body) {
        if (f_status == 200) {
            const QJsonArray l_members = f_body.array();
            for (const QJsonValue &l_value : l_members) {
                const QJsonObject l_user = l_value.toObject().value("user").toObject();
                if (l_user.value("username").toString().toLower() == f_discord_name) {
                    f_callback(l_user.value("id").toString());
                    return;
                }
            }
        }
        else {
            qWarning() << "Whitelist: member search failed. HTTP status:" << f_status;
        }
        f_callback(QString());
    });
}

QJsonObject DiscordWhitelist::embed(const QString &f_title, const QString &f_description) const
{
    return QJsonObject{
        {"title", f_title},
        {"description", f_description},
        {"color", int(QRandomGenerator::global()->bounded(0x1000000))},
    };
}

// ---------------------------------------------------------------------------------------------------------------
// Whitelisting
// ---------------------------------------------------------------------------------------------------------------

void DiscordWhitelist::requestWhitelist(AOClient *f_client, const QString &f_discord_name)
{
    QPointer<AOClient> l_client(f_client);
    const QString l_name = f_discord_name.trimmed().toLower();
    const QString l_server_name = ConfigManager::serverName();
    const QString l_character = f_client->character();

    auto l_fail = [l_client](const QString &f_message) {
        if (l_client) {
            l_client->m_whitelist_requested = false; // lets the player correct a typo without rejoining
            l_client->sendServerMessage(f_message);
        }
    };

    if (!m_ready) {
        l_fail("ERR: The whitelist bot is not connected to Discord right now. Please contact " + l_server_name + " Staff.");
        return;
    }

    ensureGuild([=](bool f_ok) {
        if (!f_ok) {
            l_fail("ERR: Whitelist Channel in Discord Server not found. Please contact " + l_server_name + " Staff.");
            return;
        }
        findMember(l_name, [=](const QString &f_member_id) {
            if (f_member_id.isEmpty()) {
                l_fail(l_name + " is not an existing member in the " + l_server_name + " Discord Server.");
                return;
            }

            const QString l_custom_id = "akashi_wl_" + QString::number(++m_prompt_counter);
            const int l_timeout = ConfigManager::whitelistPromptTimeout();
            QJsonObject l_embed = embed("Waiting to whitelist...", l_character + " (<@" + f_member_id + ">) has requested to be whitelisted!\nPlease press the button to confirm this action.");
            l_embed["fields"] = QJsonArray{QJsonObject{
                {"name", "Prompt Ends In:"},
                {"value", "<t:" + QString::number(QDateTime::currentSecsSinceEpoch() + l_timeout) + ":R>"},
                {"inline", true},
            }};
            const QJsonObject l_message{
                {"embeds", QJsonArray{l_embed}},
                {"components", QJsonArray{QJsonObject{
                                  {"type", 1},
                                  {"components", QJsonArray{QJsonObject{
                                                     {"type", 2},
                                                     {"style", 1},
                                                     {"label", "Whitelist Self"},
                                                     {"custom_id", l_custom_id},
                                                 }}},
                              }}},
            };

            Prompt *l_prompt = new Prompt;
            l_prompt->member_id = f_member_id;
            l_prompt->discord_name = l_name;
            l_prompt->timer = new QTimer(this);
            l_prompt->timer->setSingleShot(true);
            connect(l_prompt->timer, &QTimer::timeout, this, [this, l_custom_id]() { expirePrompt(l_custom_id); });
            m_prompts.insert(l_custom_id, l_prompt);

            api("POST", "/channels/" + m_channel_id + "/messages", l_message, [=](int f_status, const QJsonDocument &f_body) {
                if (f_status != 200 && f_status != 201) {
                    qWarning() << "Whitelist: could not post the prompt. HTTP status:" << f_status;
                    delete m_prompts.take(l_custom_id);
                    l_fail("ERR: Whitelist Channel in Discord Server not found. Please contact " + l_server_name + " Staff.");
                    return;
                }
                Prompt *l_stored = m_prompts.value(l_custom_id, nullptr);
                if (l_stored) {
                    l_stored->message_id = f_body.object().value("id").toString();
                    l_stored->timer->start(l_timeout * 1000);
                }
            });
        });
    });
}

void DiscordWhitelist::announceReturningUser(const QString &f_discord_name)
{
    if (!m_ready) {
        return;
    }
    const QString l_name = f_discord_name.trimmed().toLower();
    ensureGuild([=](bool f_ok) {
        if (!f_ok) {
            return;
        }
        findMember(l_name, [=](const QString &f_member_id) {
            if (f_member_id.isEmpty()) {
                return;
            }
            const QJsonObject l_message{{"embeds", QJsonArray{embed("Returning User!", "Welcome back to " + ConfigManager::serverName() + ", <@" + f_member_id + ">!")}}};
            api("POST", "/channels/" + m_channel_id + "/messages", l_message);
        });
    });
}

void DiscordWhitelist::expirePrompt(const QString &f_custom_id)
{
    Prompt *l_prompt = m_prompts.take(f_custom_id);
    if (!l_prompt) {
        return;
    }
    if (!l_prompt->done && !l_prompt->message_id.isEmpty()) {
        const QString l_path = "/channels/" + m_channel_id + "/messages/" + l_prompt->message_id;
        const QJsonObject l_edit{
            {"embeds", QJsonArray{embed("Whitelist Timed Out!", "Retry the Whitelist Procedure through " + ConfigManager::serverName() + ".")}},
            {"components", QJsonArray()},
        };
        api("PATCH", l_path, l_edit);
        QTimer::singleShot(120 * 1000, this, [this, l_path]() { api("DELETE", l_path, QJsonObject()); });
    }
    l_prompt->timer->deleteLater();
    delete l_prompt;
}

int DiscordWhitelist::whitelistClients(const QString &f_discord_name)
{
    int l_count = 0;
    const QVector<AOClient *> l_clients = m_server->getClients();
    for (AOClient *l_client : l_clients) {
        if (l_client->m_discord_name == f_discord_name && !l_client->m_whitelisted && l_client->m_whitelist_requested) {
            l_client->m_whitelisted = true;
            l_client->sendServerMessage("Whitelisted as " + f_discord_name + ".");
            m_server->addTrustedUser(f_discord_name, l_client->getIpid());
            l_count++;
        }
    }
    return l_count;
}

void DiscordWhitelist::handleInteraction(const QJsonObject &f_data)
{
    // Only button presses (type 3) are interesting.
    if (f_data.value("type").toInt() != 3) {
        return;
    }
    const QString l_custom_id = f_data.value("data").toObject().value("custom_id").toString();
    Prompt *l_prompt = m_prompts.value(l_custom_id, nullptr);
    if (!l_prompt) {
        return;
    }

    const QJsonObject l_user = f_data.value("member").toObject().value("user").toObject();
    const QString l_user_id = l_user.value("id").toString();
    const QString l_server_name = ConfigManager::serverName();

    if (l_user_id != l_prompt->member_id) {
        // Somebody else pressed the button of this prompt.
        respondToInteraction(f_data, 4, QJsonObject{{"content", "**Shoot yourself <@" + l_user_id + "> (in-game) for pressing " + l_prompt->discord_name + "'s Prompt**"}}, 5);
        return;
    }

    if (whitelistClients(l_prompt->discord_name) == 0) {
        respondToInteraction(f_data, 4, QJsonObject{{"content", "Your client is not found in " + l_server_name + "!"}, {"flags", 64}}, 15);
        return;
    }

    l_prompt->done = true;
    respondToInteraction(f_data, 7, QJsonObject{{"embeds", QJsonArray{embed("Successfully whitelisted!", "Welcome to " + l_server_name + ", <@" + l_user_id + ">!")}}, {"components", QJsonArray()}});
    expirePrompt(l_custom_id);
}

void DiscordWhitelist::respondToInteraction(const QJsonObject &f_interaction, int f_type, const QJsonObject &f_data, int f_delete_after)
{
    const QString l_id = f_interaction.value("id").toString();
    const QString l_token = f_interaction.value("token").toString();
    api("POST", "/interactions/" + l_id + "/" + l_token + "/callback", QJsonObject{{"type", f_type}, {"data", f_data}});

    if (f_delete_after > 0 && !m_application_id.isEmpty()) {
        const QString l_path = "/webhooks/" + m_application_id + "/" + l_token + "/messages/@original";
        QTimer::singleShot(f_delete_after * 1000, this, [this, l_path]() { api("DELETE", l_path, QJsonObject()); });
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------------------------------------------

void DiscordWhitelist::connectGateway()
{
    if (m_stopped) {
        return;
    }
    m_ready = false;
    m_sequence = -1;
    m_socket.open(QUrl(m_gateway_url + "/?v=10&encoding=json"));
}

void DiscordWhitelist::scheduleReconnect()
{
    if (m_stopped) {
        return;
    }
    qInfo() << "Whitelist: reconnecting to Discord in" << m_reconnect_delay << "seconds.";
    m_reconnect_timer.start(m_reconnect_delay * 1000);
    m_reconnect_delay = qMin(m_reconnect_delay * 2, 60);
}

void DiscordWhitelist::onGatewayConnected()
{
    // The bot identifies itself once the gateway has said hello.
}

void DiscordWhitelist::onGatewayDisconnected()
{
    m_heartbeat_timer.stop();
    m_ready = false;

    const int l_code = m_socket.closeCode();
    if (isFatalCloseCode(l_code)) {
        m_stopped = true;
        qWarning().noquote() << "Whitelist: the Discord bot cannot continue. " + describeCloseCode(l_code);
        return;
    }
    scheduleReconnect();
}

void DiscordWhitelist::sendGateway(const QJsonObject &f_payload)
{
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(f_payload).toJson(QJsonDocument::Compact)));
}

void DiscordWhitelist::sendHeartbeat()
{
    sendGateway(QJsonObject{{"op", 1}, {"d", m_sequence < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(m_sequence)}});
}

void DiscordWhitelist::onHeartbeatTimer()
{
    if (!m_heartbeat_acked) {
        // Discord did not answer the last heartbeat, the connection is dead.
        qWarning() << "Whitelist: Discord stopped answering, reconnecting.";
        m_socket.close(QWebSocketProtocol::CloseCodeAbnormalDisconnection);
        return;
    }
    m_heartbeat_acked = false;
    sendHeartbeat();
}

void DiscordWhitelist::identify()
{
    // Intents: GUILDS (1) and GUILD_MEMBERS (2), the latter is needed to look players up by name.
    sendGateway(QJsonObject{
        {"op", 2},
        {"d", QJsonObject{
                  {"token", m_token},
                  {"intents", 3},
                  {"properties", QJsonObject{{"os", "linux"}, {"browser", "akashi"}, {"device", "akashi"}}},
              }},
    });
}

void DiscordWhitelist::onGatewayMessage(const QString &f_message)
{
    const QJsonObject l_payload = QJsonDocument::fromJson(f_message.toUtf8()).object();
    const int l_op = l_payload.value("op").toInt(-1);
    if (l_payload.contains("s") && !l_payload.value("s").isNull()) {
        m_sequence = qint64(l_payload.value("s").toDouble());
    }

    switch (l_op) {
    case 0: // Dispatch
        handleDispatch(l_payload.value("t").toString(), l_payload.value("d").toObject());
        break;
    case 1: // The gateway wants a heartbeat right now
        sendHeartbeat();
        break;
    case 7: // Reconnect
    case 9: // Invalid session
        m_socket.close(QWebSocketProtocol::CloseCodeGoingAway);
        break;
    case 10: { // Hello
        const int l_interval = l_payload.value("d").toObject().value("heartbeat_interval").toInt(41250);
        m_heartbeat_acked = true;
        m_heartbeat_timer.start(l_interval);
        QTimer::singleShot(int(l_interval * QRandomGenerator::global()->generateDouble()), this, [this]() {
            if (m_socket.state() == QAbstractSocket::ConnectedState) {
                sendHeartbeat();
            }
        });
        identify();
        break;
    }
    case 11: // Heartbeat acknowledged
        m_heartbeat_acked = true;
        break;
    default:
        break;
    }
}

void DiscordWhitelist::handleDispatch(const QString &f_event, const QJsonObject &f_data)
{
    if (f_event == "READY") {
        m_ready = true;
        m_reconnect_delay = 5;
        m_application_id = f_data.value("application").toObject().value("id").toString();
        m_bot_name = f_data.value("user").toObject().value("username").toString();
        qInfo().noquote() << "Whitelist: Discord bot connected as" << m_bot_name;
    }
    else if (f_event == "INTERACTION_CREATE") {
        handleInteraction(f_data);
    }
}
