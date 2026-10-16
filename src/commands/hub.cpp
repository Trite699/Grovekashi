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
#include "aoclient.h"

#include "area_data.h"
#include "config_manager.h"
#include "server.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
const int MAX_HUB_AREAS = 5;
const int MAX_AREA_NAME_LENGTH = 25;
const int MAX_HUB_FILE_NAME_LENGTH = 20;

/**
 * @brief Checks a new name for an area.
 *
 * @return An empty string if the name is fine, otherwise what is wrong with it.
 */
QString checkAreaName(Server *f_server, const QString &f_name, int f_ignored_index = -1)
{
    if (f_name.isEmpty()) {
        return "You must specify a name.";
    }
    if (f_name.length() > MAX_AREA_NAME_LENGTH) {
        return "That name is too long!";
    }
    static const QString l_allowed_symbols = ":!-? ";
    for (const QChar &l_char : f_name) {
        if (!l_char.isLetterOrNumber() && !l_allowed_symbols.contains(l_char)) {
            return "Try to exclude special characters while renaming.";
        }
    }
    for (int i = 0; i < f_server->getAreaCount(); i++) {
        if (i != f_ignored_index && f_server->getAreaName(i).compare(f_name, Qt::CaseInsensitive) == 0) {
            return "This name is already taken.";
        }
    }
    return QString();
}

int dynamicAreaCount(Server *f_server, const QString &f_hub)
{
    int l_count = 0;
    const QList<int> l_ids = f_server->getHubAreaIds(f_hub);
    for (int l_id : l_ids) {
        if (f_server->getAreaById(l_id)->isDynamic()) {
            l_count++;
        }
    }
    return l_count;
}

/**
 * @brief Finds the first name of the form "Area N" that is not used yet.
 */
QString nextFreeAreaName(Server *f_server, int f_start)
{
    int l_number = qMax(1, f_start);
    while (!checkAreaName(f_server, "Area " + QString::number(l_number)).isEmpty()) {
        l_number++;
    }
    return "Area " + QString::number(l_number);
}

/**
 * @brief Removes every area that was created in a hub and clears the connections of the others.
 *
 * @return The number of removed areas.
 */
int clearHubAreas(Server *f_server, const QString &f_hub)
{
    int l_removed = 0;
    // Backwards, because removing an area moves the indices of the ones behind it.
    for (int i = f_server->getAreaCount() - 1; i >= 0; i--) {
        AreaData *l_area = f_server->getAreaById(i);
        if (l_area->isDynamic() && l_area->hub().compare(f_hub, Qt::CaseInsensitive) == 0) {
            if (f_server->removeDynamicArea(i)) {
                l_removed++;
            }
        }
    }
    const QList<int> l_ids = f_server->getHubAreaIds(f_hub);
    for (int l_id : l_ids) {
        f_server->getAreaById(l_id)->clearConnections();
        f_server->getAreaById(l_id)->setMoveTime(0);
    }
    return l_removed;
}

/**
 * @brief Turns "3 5" or "Night Resort" into areas of the given hub.
 *
 * @details Numbers are area IDs, anything else is read as one area name.
 */
QList<AreaData *> findAreas(Server *f_server, const QStringList &f_arguments, const QString &f_hub, QString &f_error)
{
    QList<AreaData *> l_found;
    bool l_all_numbers = true;
    for (const QString &l_argument : f_arguments) {
        bool l_ok;
        l_argument.toInt(&l_ok);
        l_all_numbers = l_all_numbers && l_ok;
    }

    QStringList l_wanted;
    if (l_all_numbers) {
        l_wanted = f_arguments;
    }
    else {
        l_wanted.append(f_arguments.join(" "));
    }

    for (const QString &l_wanted_area : qAsConst(l_wanted)) {
        AreaData *l_area = nullptr;
        bool l_is_number;
        const int l_id = l_wanted_area.toInt(&l_is_number);
        if (l_is_number) {
            if (l_id >= 0 && l_id < f_server->getAreaCount()) {
                l_area = f_server->getAreaById(l_id);
            }
        }
        else {
            for (int i = 0; i < f_server->getAreaCount(); i++) {
                if (f_server->getAreaName(i).compare(l_wanted_area, Qt::CaseInsensitive) == 0) {
                    l_area = f_server->getAreaById(i);
                    break;
                }
            }
        }
        if (!l_area) {
            f_error = "Could not find the area \"" + l_wanted_area + "\".";
            return QList<AreaData *>();
        }
        if (l_area->hub().compare(f_hub, Qt::CaseInsensitive) != 0) {
            f_error = l_area->name() + " is not in the same hub.";
            return QList<AreaData *>();
        }
        l_found.append(l_area);
    }
    return l_found;
}

QString hubFilePath(const QString &f_name)
{
    return "storage/hubs/" + f_name + ".json";
}

bool validHubFileName(const QString &f_name)
{
    if (f_name.isEmpty() || f_name.length() > MAX_HUB_FILE_NAME_LENGTH) {
        return false;
    }
    for (const QChar &l_char : f_name) {
        if (!l_char.isLetterOrNumber() && l_char != '_' && l_char != '-') {
            return false;
        }
    }
    return true;
}
} // namespace

void AOClient::cmdAddArea(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only create areas in hubs.");
        return;
    }
    if (dynamicAreaCount(server, l_area->hub()) >= MAX_HUB_AREAS) {
        sendServerMessage("You cannot have more than " + QString::number(MAX_HUB_AREAS) + " created areas in a hub.");
        return;
    }

    QString l_name = argv.join(" ").trimmed();
    if (l_name.isEmpty()) {
        l_name = nextFreeAreaName(server, dynamicAreaCount(server, l_area->hub()) + 1);
    }
    const QString l_problem = checkAreaName(server, l_name);
    if (!l_problem.isEmpty()) {
        sendServerMessage(l_problem);
        return;
    }

    const int l_index = server->addDynamicArea(l_name, l_area);
    sendServerMessage("Area created: [" + QString::number(l_index) + "] " + l_name);
}

void AOClient::cmdAddAreas(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only create areas in hubs.");
        return;
    }

    bool l_ok;
    const int l_amount = argv[0].toInt(&l_ok);
    if (!l_ok) {
        sendServerMessage("That does not look like a valid number.");
        return;
    }
    if (l_amount < 1) {
        sendServerMessage("You must create at least 1 area.");
        return;
    }
    const int l_existing = dynamicAreaCount(server, l_area->hub());
    if (l_existing + l_amount > MAX_HUB_AREAS) {
        sendServerMessage("You cannot have more than " + QString::number(MAX_HUB_AREAS) + " created areas in a hub.");
        return;
    }

    int l_next = l_existing + 1;
    for (int i = 0; i < l_amount; i++) {
        const QString l_name = nextFreeAreaName(server, l_next);
        server->addDynamicArea(l_name, l_area);
        l_next++;
    }
    sendServerMessage("Created " + QString::number(l_amount) + " area(s).");
}

void AOClient::cmdRemoveArea(int argc, QStringList argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    AreaData *l_area = server->getAreaById(areaId());
    if (!l_area->isDynamic()) {
        sendServerMessage("You can only remove areas that were created with /addarea or /loadhub.");
        return;
    }
    const QString l_name = l_area->name();
    server->removeDynamicArea(areaId());
    sendServerMessage("Removed the area " + l_name + ".");
}

void AOClient::cmdRename(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only rename areas in hubs.");
        return;
    }

    const QString l_name = argv.join(" ").trimmed();
    const QString l_problem = checkAreaName(server, l_name, areaId());
    if (!l_problem.isEmpty()) {
        sendServerMessage(l_problem);
        return;
    }

    const QString l_old_name = l_area->name();
    server->renameArea(areaId(), l_name);
    sendServerMessageArea(character() + " renamed the area " + l_old_name + " to " + l_name + ".");
}

void AOClient::cmdClearHub(int argc, QStringList argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    const QString l_hub = server->getAreaById(areaId())->hub();
    if (l_hub.isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }

    const int l_removed = clearHubAreas(server, l_hub);
    sendServerMessage("Cleared the hub " + l_hub + ": removed " + QString::number(l_removed) + " area(s) and all connections.");
}

void AOClient::cmdSaveHub(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    const QString l_hub = server->getAreaById(areaId())->hub();
    if (l_hub.isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }
    const QString l_name = argv.join(" ").trimmed();
    if (!validHubFileName(l_name)) {
        sendServerMessage("The name of a saved hub can only have letters, numbers, _ and -, and at most " + QString::number(MAX_HUB_FILE_NAME_LENGTH) + " characters.");
        return;
    }

    QJsonArray l_areas;
    const QList<int> l_ids = server->getHubAreaIds(l_hub);
    for (int l_id : l_ids) {
        AreaData *l_area = server->getAreaById(l_id);
        if (!l_area->isDynamic()) {
            continue;
        }
        QJsonArray l_connections;
        const QList<AreaData *> l_area_connections = l_area->connections();
        for (AreaData *l_connection : l_area_connections) {
            l_connections.append(l_connection->name());
        }
        l_areas.append(QJsonObject{
            {"name", l_area->name()},
            {"background", l_area->background()},
            {"document", l_area->document()},
            {"area_message", l_area->areaMessage()},
            {"move_time", l_area->moveTime()},
            {"connections", l_connections},
        });
    }
    if (l_areas.isEmpty()) {
        sendServerMessage("There are no created areas in this hub to save.");
        return;
    }

    QDir().mkpath("storage/hubs");
    QFile l_file(hubFilePath(l_name));
    if (!l_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sendServerMessage("Could not save the hub. Does the server have write access to the storage folder?");
        return;
    }
    l_file.write(QJsonDocument(QJsonObject{{"areas", l_areas}}).toJson());
    sendServerMessage("Saved " + QString::number(l_areas.size()) + " area(s) as the hub \"" + l_name + "\".");
}

void AOClient::cmdLoadHub(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_template = server->getAreaById(areaId());
    const QString l_hub = l_template->hub();
    if (l_hub.isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }
    const QString l_name = argv.join(" ").trimmed();
    if (!validHubFileName(l_name)) {
        sendServerMessage("That is not a valid name for a saved hub.");
        return;
    }
    QFile l_file(hubFilePath(l_name));
    if (!l_file.open(QIODevice::ReadOnly)) {
        sendServerMessage("No hub with that name found.");
        return;
    }
    const QJsonArray l_areas = QJsonDocument::fromJson(l_file.readAll()).object().value("areas").toArray();
    if (l_areas.isEmpty() || l_areas.size() > MAX_HUB_AREAS) {
        sendServerMessage("That saved hub is empty or has more than " + QString::number(MAX_HUB_AREAS) + " areas.");
        return;
    }

    // The loaded hub replaces what was created in this hub.
    clearHubAreas(server, l_hub);
    l_template = server->getAreaById(server->getHubAreaIds(l_hub).value(0, 0));

    QList<AreaData *> l_created;
    int l_skipped = 0;
    for (const QJsonValue &l_value : l_areas) {
        const QJsonObject l_object = l_value.toObject();
        const QString l_area_name = l_object.value("name").toString();
        if (!checkAreaName(server, l_area_name).isEmpty()) {
            l_created.append(nullptr);
            l_skipped++;
            continue;
        }
        const int l_index = server->addDynamicArea(l_area_name, l_template);
        AreaData *l_area = server->getAreaById(l_index);
        if (!l_object.value("background").toString().isEmpty()) {
            l_area->setBackground(l_object.value("background").toString());
        }
        l_area->changeDoc(l_object.value("document").toString("No document."));
        l_area->changeAreaMessage(l_object.value("area_message").toString());
        l_area->setMoveTime(l_object.value("move_time").toInt());
        l_created.append(l_area);
    }

    // Connections refer to areas by name, so they can only be restored once all areas exist.
    for (int i = 0; i < l_areas.size(); i++) {
        if (!l_created.at(i)) {
            continue;
        }
        const QJsonArray l_connections = l_areas.at(i).toObject().value("connections").toArray();
        for (const QJsonValue &l_connection : l_connections) {
            for (int l_id = 0; l_id < server->getAreaCount(); l_id++) {
                if (server->getAreaName(l_id).compare(l_connection.toString(), Qt::CaseInsensitive) == 0 && server->getAreaById(l_id)->hub().compare(l_hub, Qt::CaseInsensitive) == 0) {
                    l_created.at(i)->addConnection(server->getAreaById(l_id));
                }
            }
        }
    }

    QString l_message = "Loaded the hub \"" + l_name + "\" with " + QString::number(l_areas.size() - l_skipped) + " area(s).";
    if (l_skipped > 0) {
        l_message += " " + QString::number(l_skipped) + " could not be created because their name is invalid or already taken.";
    }
    sendServerMessage(l_message);
}

void AOClient::cmdConnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only connect areas in hubs.");
        return;
    }
    QString l_error;
    const QList<AreaData *> l_targets = findAreas(server, argv, l_area->hub(), l_error);
    if (!l_error.isEmpty()) {
        sendServerMessage(l_error);
        return;
    }
    for (AreaData *l_target : l_targets) {
        if (l_target == l_area) {
            sendServerMessage("You cannot connect an area to itself.");
            return;
        }
    }
    for (AreaData *l_target : l_targets) {
        l_area->addConnection(l_target);
    }
    sendServerMessage("Connected " + l_area->name() + " to " + QString::number(l_targets.size()) + " area(s).");
}

void AOClient::cmdBiConnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only connect areas in hubs.");
        return;
    }
    QString l_error;
    const QList<AreaData *> l_targets = findAreas(server, argv, l_area->hub(), l_error);
    if (!l_error.isEmpty()) {
        sendServerMessage(l_error);
        return;
    }
    for (AreaData *l_target : l_targets) {
        if (l_target == l_area) {
            sendServerMessage("You cannot connect an area to itself.");
            return;
        }
    }
    for (AreaData *l_target : l_targets) {
        l_area->addConnection(l_target);
        l_target->addConnection(l_area);
    }
    sendServerMessage("Connected " + l_area->name() + " both ways to " + QString::number(l_targets.size()) + " area(s).");
}

void AOClient::cmdDisconnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only disconnect areas in hubs.");
        return;
    }
    QString l_error;
    const QList<AreaData *> l_targets = findAreas(server, argv, l_area->hub(), l_error);
    if (!l_error.isEmpty()) {
        sendServerMessage(l_error);
        return;
    }
    int l_removed = 0;
    for (AreaData *l_target : l_targets) {
        if (l_area->connections().contains(l_target)) {
            l_area->removeConnection(l_target);
            l_removed++;
        }
    }
    sendServerMessage("Removed " + QString::number(l_removed) + " connection(s) from " + l_area->name() + ".");
}

void AOClient::cmdBiDisconnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only disconnect areas in hubs.");
        return;
    }
    QString l_error;
    const QList<AreaData *> l_targets = findAreas(server, argv, l_area->hub(), l_error);
    if (!l_error.isEmpty()) {
        sendServerMessage(l_error);
        return;
    }
    int l_removed = 0;
    for (AreaData *l_target : l_targets) {
        if (l_area->connections().contains(l_target)) {
            l_area->removeConnection(l_target);
            l_removed++;
        }
        if (l_target->connections().contains(l_area)) {
            l_target->removeConnection(l_area);
            l_removed++;
        }
    }
    sendServerMessage("Removed " + QString::number(l_removed) + " connection(s) between " + l_area->name() + " and the other area(s).");
}

void AOClient::cmdConnectList(int argc, QStringList argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }
    if (!l_area->isRestricted()) {
        sendServerMessage("[" + QString::number(l_area->index()) + "] " + l_area->name() + " has no connections, so players can go anywhere from here.");
        return;
    }
    QStringList l_names;
    const QList<AreaData *> l_connections = l_area->connections();
    for (AreaData *l_connection : l_connections) {
        l_names.append("[" + QString::number(l_connection->index()) + "] " + l_connection->name());
    }
    sendServerMessage("[" + QString::number(l_area->index()) + "] " + l_area->name() + " is connected to: " + l_names.join(", "));
}

void AOClient::cmdClearConnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }
    l_area->clearConnections();
    sendServerMessage("Removed all connections of " + l_area->name() + ".");
}

void AOClient::cmdHubClearConnect(int argc, QStringList argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    const QString l_hub = server->getAreaById(areaId())->hub();
    if (l_hub.isEmpty()) {
        sendServerMessage("This area is not part of a hub.");
        return;
    }
    const QList<int> l_ids = server->getHubAreaIds(l_hub);
    for (int l_id : l_ids) {
        server->getAreaById(l_id)->clearConnections();
    }
    sendServerMessage("Removed all connections in the hub " + l_hub + ".");
}

void AOClient::cmdMoveTime(int argc, QStringList argv)
{
    AreaData *l_area = server->getAreaById(areaId());
    if (l_area->hub().isEmpty()) {
        sendServerMessage("You can only use this in a hub.");
        return;
    }

    if (argc == 0) {
        if (l_area->moveTime() > 0) {
            sendServerMessage("It takes " + QString::number(l_area->moveTime()) + " seconds to move to " + l_area->name() + ".");
        }
        else {
            sendServerMessage("There is no wait to move to " + l_area->name() + ".");
        }
        return;
    }

    if (!checkPermission(ACLRole::CM)) {
        sendServerMessage("You must be a CM to change the move time.");
        return;
    }
    bool l_ok;
    const int l_seconds = argv[0].toInt(&l_ok);
    if (!l_ok || l_seconds < 0 || l_seconds > 600) {
        sendServerMessage("The move time must be a number of seconds between 0 and 600.");
        return;
    }
    l_area->setMoveTime(l_seconds);
    sendServerMessageArea(l_seconds == 0 ? "Players can now move to " + l_area->name() + " without waiting." : "It now takes " + QString::number(l_seconds) + " seconds to move to " + l_area->name() + ".");
}
