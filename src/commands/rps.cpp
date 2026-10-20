#include "aoclient.h"

#include <QDateTime>

#include "server.h"

QString AOClient::rpsChoiceName(RpsChoice f_choice)
{
    switch (f_choice) {
    case RpsChoice::ROCK:
        return QStringLiteral("Rock");
    case RpsChoice::PAPER:
        return QStringLiteral("Paper");
    case RpsChoice::SCISSORS:
        return QStringLiteral("Scissors");
    default:
        return QStringLiteral("nothing");
    }
}

QString AOClient::rpsDisplayName() const
{
    QString l_name = name();
    if (l_name.isEmpty()) {
        l_name = character();
    }
    if (l_name.isEmpty()) {
        l_name = QStringLiteral("User");
    }
    return "[" + QString::number(clientId()) + "] " + l_name;
}

bool AOClient::rpsActive() const
{
    return m_rps_opponent != -1 && (QDateTime::currentSecsSinceEpoch() - m_rps_started) < RPS_TIMEOUT_SECS;
}

void AOClient::rpsReset()
{
    m_rps_opponent = -1;
    m_rps_choice = RpsChoice::NONE;
    m_rps_started = 0;
}

void AOClient::rpsAbort(const QString &f_reason)
{
    if (m_rps_opponent == -1) {
        return;
    }

    // An expired game is cleaned up silently, there is nothing left to tell anyone.
    const bool l_notify = rpsActive();
    AOClient *l_opponent = server->getClientByID(m_rps_opponent);
    if (l_opponent != nullptr && l_opponent->m_rps_opponent == clientId()) {
        l_opponent->rpsReset();
        if (l_notify) {
            l_opponent->sendServerMessage(f_reason);
        }
    }
    rpsReset();
}

AOClient::RpsChoice AOClient::rpsParseChoice(const QString &f_text)
{
    const QString l_text = f_text.trimmed().toLower();
    if (l_text == "r" || l_text == "rock") {
        return RpsChoice::ROCK;
    }
    if (l_text == "p" || l_text == "paper") {
        return RpsChoice::PAPER;
    }
    if (l_text == "s" || l_text == "scissors") {
        return RpsChoice::SCISSORS;
    }
    return RpsChoice::NONE;
}

void AOClient::cmdRps(int argc, QStringList argv)
{
    Q_UNUSED(argc);

    // "/rps r|p|s" picks an action, "/rps <user id>" starts a game.
    const RpsChoice l_choice = rpsParseChoice(argv[0]);
    if (l_choice != RpsChoice::NONE) {
        rpsPick(l_choice);
        return;
    }

    bool l_ok = false;
    const int l_target_id = argv[0].toInt(&l_ok);
    if (!l_ok) {
        sendServerMessage("Usage: /rps <user id> to challenge someone, then /rps <r|p|s> (rock, paper, scissors) to choose.");
        return;
    }
    if (l_target_id == clientId()) {
        sendServerMessage("You cannot play Rock Paper Scissors against yourself.");
        return;
    }

    AOClient *l_target = server->getClientByID(l_target_id);
    if (l_target == nullptr || !l_target->hasJoined()) {
        sendServerMessage("No user with that ID found.");
        return;
    }

    // Do not let a challenge steamroll somebody else's running game.
    if (l_target->rpsActive() && l_target->m_rps_opponent != clientId()) {
        sendServerMessage(l_target->rpsDisplayName() + " is already playing Rock Paper Scissors.");
        return;
    }

    // Starting a game drops whatever game this client had open, and clears out expired leftovers of the target.
    rpsAbort(rpsDisplayName() + " started a new Rock Paper Scissors game, so your old one was cancelled.");
    l_target->rpsAbort(QString());

    const qint64 l_now = QDateTime::currentSecsSinceEpoch();
    m_rps_opponent = l_target_id;
    m_rps_choice = RpsChoice::NONE;
    m_rps_started = l_now;
    l_target->m_rps_opponent = clientId();
    l_target->m_rps_choice = RpsChoice::NONE;
    l_target->m_rps_started = l_now;

    const QString l_prompt = "Select your action: /rps r (Rock), /rps p (Paper) or /rps s (Scissors)";
    sendServerMessage("You challenged " + l_target->rpsDisplayName() + " to Rock Paper Scissors!\n" + l_prompt);
    l_target->sendServerMessage(rpsDisplayName() + " challenged you to Rock Paper Scissors!\n" + l_prompt);
}

void AOClient::rpsPick(RpsChoice f_choice)
{
    if (m_rps_opponent == -1) {
        sendServerMessage("You are not in a Rock Paper Scissors game. Start one with /rps <user id>.");
        return;
    }
    if (!rpsActive()) {
        rpsAbort(QString());
        sendServerMessage("Your Rock Paper Scissors game expired. Start a new one with /rps <user id>.");
        return;
    }

    AOClient *l_opponent = server->getClientByID(m_rps_opponent);
    if (l_opponent == nullptr || l_opponent->m_rps_opponent != clientId()) {
        rpsReset();
        sendServerMessage("Your opponent is no longer available, so the game was cancelled.");
        return;
    }

    if (m_rps_choice != RpsChoice::NONE) {
        sendServerMessage("You already selected " + rpsChoiceName(m_rps_choice) + ". Waiting for " + l_opponent->rpsDisplayName() + " to choose.");
        return;
    }

    m_rps_choice = f_choice;

    // Nothing is revealed until both players have chosen.
    if (l_opponent->m_rps_choice == RpsChoice::NONE) {
        sendServerMessage("You selected " + rpsChoiceName(f_choice) + ". Waiting for " + l_opponent->rpsDisplayName() + " to choose.");
        l_opponent->sendServerMessage(rpsDisplayName() + " has made their choice. Select your action: /rps r (Rock), /rps p (Paper) or /rps s (Scissors)");
        return;
    }

    const RpsChoice l_mine = m_rps_choice;
    const RpsChoice l_theirs = l_opponent->m_rps_choice;

    QString l_result;
    if (l_mine == l_theirs) {
        l_result = rpsDisplayName() + " and " + l_opponent->rpsDisplayName() + " both chose " + rpsChoiceName(l_mine) + ". It's a draw!";
    }
    else {
        // Rock = 1, Paper = 2, Scissors = 3: each choice beats the one right below it, and Rock beats Scissors.
        const bool l_i_win = (static_cast<int>(l_mine) - static_cast<int>(l_theirs) + 3) % 3 == 1;
        AOClient *l_winner = l_i_win ? this : l_opponent;
        AOClient *l_loser = l_i_win ? l_opponent : this;
        l_result = l_loser->rpsDisplayName() + " chose " + rpsChoiceName(l_loser->m_rps_choice) + " and gets beaten by " + rpsChoiceName(l_winner->m_rps_choice) + " chosen by " + l_winner->rpsDisplayName() + "!";
    }

    sendServerMessage(l_result);
    l_opponent->sendServerMessage(l_result);

    l_opponent->rpsReset();
    rpsReset();
}
