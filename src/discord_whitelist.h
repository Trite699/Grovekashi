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
#ifndef DISCORD_WHITELIST_H
#define DISCORD_WHITELIST_H

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWebSocket>
#include <functional>

class AOClient;
class Server;

/**
 * @brief A Discord bot that lets players whitelist themselves, modelled on the whitelist of tsuserverCC.
 *
 * @details A player types /whitelist <discord name> in game. The bot looks that name up in the Discord server,
 * posts a message with a "Whitelist Self" button in the whitelist channel, and when that Discord user presses the
 * button, every client of the player that asked is whitelisted. The bot talks to Discord directly: it listens for
 * the button presses on the gateway and answers through the REST API.
 */
class DiscordWhitelist : public QObject
{
    Q_OBJECT

  public:
    /**
     * @brief Creates the bot. It does nothing until start() is called.
     */
    explicit DiscordWhitelist(Server *f_server, QObject *parent = nullptr);

    /**
     * @brief Connects to the Discord gateway.
     */
    void start();

    /**
     * @brief Posts a whitelist prompt for a player into the whitelist channel.
     *
     * @param f_client The player who asked to be whitelisted. Told about the result when something goes wrong.
     * @param f_discord_name The Discord username the player gave, lowercase.
     */
    void requestWhitelist(AOClient *f_client, const QString &f_discord_name);

    /**
     * @brief Greets a returning, already trusted player in the whitelist channel.
     */
    void announceReturningUser(const QString &f_discord_name);

    /**
     * @brief Returns true if the bot is connected to the gateway.
     */
    bool isConnected() const;

  private slots:
    void onGatewayConnected();
    void onGatewayMessage(const QString &f_message);
    void onGatewayDisconnected();
    void onHeartbeatTimer();

  private:
    /**
     * @brief A whitelist prompt that is waiting for its button to be pressed.
     */
    struct Prompt
    {
        QString member_id;
        QString discord_name;
        QString message_id;
        QTimer *timer = nullptr;
        bool done = false;
    };

    using ApiCallback = std::function<void(int f_status, const QJsonDocument &f_body)>;

    void api(const QByteArray &f_verb, const QString &f_path, const QJsonObject &f_body, ApiCallback f_callback = nullptr);
    void ensureGuild(std::function<void(bool f_ok)> f_callback);
    void findMember(const QString &f_discord_name, std::function<void(const QString &f_member_id)> f_callback);

    void connectGateway();
    void scheduleReconnect();
    void sendGateway(const QJsonObject &f_payload);
    void sendHeartbeat();
    void identify();
    void handleDispatch(const QString &f_event, const QJsonObject &f_data);
    void handleInteraction(const QJsonObject &f_data);
    void respondToInteraction(const QJsonObject &f_interaction, int f_type, const QJsonObject &f_data, int f_delete_after = 0);
    void expirePrompt(const QString &f_custom_id);

    /**
     * @brief Whitelists every client that asked to be whitelisted under the given Discord name.
     *
     * @return The number of clients that were whitelisted.
     */
    int whitelistClients(const QString &f_discord_name);

    QJsonObject embed(const QString &f_title, const QString &f_description) const;

    Server *m_server;
    QNetworkAccessManager m_network;
    QWebSocket m_socket;
    QTimer m_heartbeat_timer;
    QTimer m_reconnect_timer;

    QString m_token;
    QString m_channel_id;
    QString m_api_url;
    QString m_gateway_url;
    QString m_guild_id;
    QString m_application_id;
    QString m_bot_name;

    QHash<QString, Prompt *> m_prompts;
    int m_prompt_counter = 0;

    qint64 m_sequence = -1;
    bool m_heartbeat_acked = true;
    bool m_stopped = false;
    bool m_ready = false;
    int m_reconnect_delay = 5;
};

#endif // DISCORD_WHITELIST_H
