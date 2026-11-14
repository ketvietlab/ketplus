#include "teams/TeamDirectory.h"

#include "teams/TeamCommands.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <algorithm>

namespace ketplus {

TeamDirectory::TeamDirectory(QObject* parent) : QObject(parent) {}

void TeamDirectory::setTransport(Transport transport) { transport_ = std::move(transport); }

TeamCommands* TeamDirectory::commands() {
    if (commands_ == nullptr)
        commands_ = new TeamCommands(this);
    return commands_;
}

void TeamDirectory::setAvailable(const bool available) {
    if (available_ == available && !stale_)
        return;
    available_ = available;
    stale_ = false;
    if (!available_) {
        teams_.clear();
        emit changed();
        return;
    }
    refresh();
}

void TeamDirectory::setDisconnected() {
    if (!available_)
        return;
    available_ = false;
    stale_ = !teams_.isEmpty();
    ++listRequest_; // an answer still on its way is dropped
    emit changed();
}

void TeamDirectory::refresh() {
    if (!available_ || !transport_)
        return;
    const QPointer<TeamDirectory> alive(this);
    // Only the newest request counts: an older one that fails must not start a second retry.
    const int request = ++listRequest_;
    transport_(QStringLiteral("team.list"), {},
               [alive, request](const QJsonObject& result, const QString& error) {
                   if (!alive || !alive->available_ || request != alive->listRequest_)
                       return;
                   if (!error.isEmpty()) {
                       const int delay = alive->retryDelayMs_;
                       alive->retryDelayMs_ = qMin(delay * 2, longestRetryMs);
                       QTimer::singleShot(delay, alive.data(), [alive, request] {
                           if (alive && request == alive->listRequest_)
                               alive->refresh();
                       });
                       return;
                   }
                   alive->retryDelayMs_ = alive->firstRetry_;
                   alive->teams_.clear();
                   for (const auto& value : result.value(QStringLiteral("teams")).toArray()) {
                       const QJsonObject team = value.toObject();
                       alive->teams_.insert(team.value(QStringLiteral("id")).toString(), team);
                   }
                   emit alive->changed();
               });
}

void TeamDirectory::store(const QJsonObject& received) {
    const QString id = received.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    // ADR 0115: team.list and team.changed carry no tasks or decisions. The ones a page read
    // with team.get stay until it reads them again.
    QJsonObject team = received;
    if (received.value(QStringLiteral("directory")).toBool())
        if (const auto known = teams_.constFind(id); known != teams_.cend())
            for (const QString key : {QStringLiteral("tasks"), QStringLiteral("decisions"),
                                      QStringLiteral("decisions_total")})
                if (known->contains(key))
                    team.insert(key, known->value(key));
    // Events and answers can arrive in either order; the higher revision wins.
    // An answer that repeats what is known changes nothing (a page re-reads on every change).
    if (const auto it = teams_.constFind(id);
        it != teams_.cend() &&
        (*it == team || it->value(QStringLiteral("revision")).toInteger() >
                            team.value(QStringLiteral("revision")).toInteger()))
        return;
    teams_.insert(id, team);
    emit changed();
}

void TeamDirectory::applyEvent(const QString& type, const QJsonObject& payload) {
    if (!available_)
        return;
    if (type == QStringLiteral("team.changed")) {
        // ADR 0115: a team too large for one message arrives as its id; read the list again.
        if (payload.value(QStringLiteral("truncated")).toBool())
            refresh();
        else
            store(payload.value(QStringLiteral("team")).toObject());
    } else if (type == QStringLiteral("team.removed")) {
        if (teams_.remove(payload.value(QStringLiteral("team_id")).toString()) > 0)
            emit changed();
    }
}

void TeamDirectory::request(const QString& type, const QJsonObject& payload, Reply reply) {
    if (!available_ || !transport_) {
        if (reply)
            reply({}, tr("Teams need a KetPlus Daemon that supports them."));
        return;
    }
    const QPointer<TeamDirectory> alive(this);
    transport_(
        type, payload,
        [alive, type, payload, reply = std::move(reply)](const QJsonObject& result,
                                                         const QString& error) {
            if (alive && error.isEmpty()) {
                if (result.contains(QStringLiteral("team")))
                    alive->store(result.value(QStringLiteral("team")).toObject());
                if (type == QStringLiteral("team.delete") &&
                    alive->teams_.remove(payload.value(QStringLiteral("team_id")).toString()))
                    emit alive->changed();
            }
            if (reply)
                reply(result, error);
        });
}

QList<QJsonObject> TeamDirectory::teams() const {
    QList<QJsonObject> list = teams_.values();
    std::sort(list.begin(), list.end(), [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("id")).toString().toLongLong() <
               right.value(QStringLiteral("id")).toString().toLongLong();
    });
    return list;
}

QList<QJsonObject> TeamDirectory::teams(const QString& repository) const {
    const QString wanted = normalizedRepository(repository);
    QList<QJsonObject> list;
    for (const QJsonObject& team : teams())
        if (normalizedRepository(team.value(QStringLiteral("repository")).toString()) == wanted)
            list.append(team);
    return list;
}

std::optional<QJsonObject> TeamDirectory::team(const QString& teamId) const {
    const auto it = teams_.constFind(teamId);
    if (it == teams_.cend())
        return std::nullopt;
    return *it;
}

std::optional<QJsonObject> TeamDirectory::teamOf(const QString& sessionId) const {
    if (sessionId.isEmpty())
        return std::nullopt;
    for (const QJsonObject& team : teams())
        if (!member(team, sessionId).isEmpty())
            return team;
    return std::nullopt;
}

QString TeamDirectory::normalizedRepository(const QString& path) {
    if (path.isEmpty())
        return {};
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

QJsonObject TeamDirectory::member(const QJsonObject& team, const QString& sessionId) {
    for (const auto& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("session_id")).toString() == sessionId)
            return value.toObject();
    return {};
}

int TeamDirectory::count(const QJsonObject& team, const QString& role) {
    const QJsonArray members = team.value(QStringLiteral("members")).toArray();
    return static_cast<int>(std::count_if(members.begin(), members.end(), [&](const auto& value) {
        return value.toObject().value(QStringLiteral("role")).toString() == role;
    }));
}

} // namespace ketplus
