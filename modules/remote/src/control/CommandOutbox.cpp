#include "control/CommandOutbox.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace ketplus::remote {
namespace {
constexpr qsizetype maximumPending = 256;
constexpr qsizetype maximumCompleted = 1024;
constexpr qint64 maximumFileBytes = 32 * 1024 * 1024;
QString routeName(CommandOutbox::Route route) {
    return route == CommandOutbox::Route::Worktree ? QStringLiteral("worktree")
                                                   : QStringLiteral("session");
}
bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}
} // namespace

CommandOutbox::CommandOutbox(QString file) : file_(std::move(file)) {}

bool CommandOutbox::load(QString* error) {
    pending_.clear();
    completed_.clear();
    QFile file(file_);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumFileBytes)
        return fail(error, QStringLiteral("Unable to read the command outbox."));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto root = document.object();
    if (parse.error != QJsonParseError::NoError || root.value("version").toInt() != 1)
        return fail(error, QStringLiteral("The command outbox is not readable."));
    for (const auto& value : root.value("pending").toArray()) {
        const auto object = value.toObject();
        const auto route = object.value("route").toString();
        Entry entry{route == "worktree" ? Route::Worktree : Route::Session,
                    object.value("envelope").toObject(), object.value("createdAt").toInteger()};
        if ((route != "worktree" && route != "session") || entry.id().isEmpty() ||
            !entry.envelope.value("content").isObject() || isPending(entry.id()))
            return fail(error, QStringLiteral("The command outbox is not readable."));
        pending_.append(entry);
    }
    for (const auto& value : root.value("completed").toArray())
        if (!value.toString().isEmpty())
            completed_.append(value.toString());
    return true;
}

bool CommandOutbox::isPending(const QString& id) const {
    return std::any_of(pending_.cbegin(), pending_.cend(),
                       [&id](const Entry& entry) { return entry.id() == id; });
}

bool CommandOutbox::isCompleted(const QString& id) const { return completed_.contains(id); }

bool CommandOutbox::add(Route route, const QJsonObject& envelope, QString* error) {
    const auto id = envelope.value("id").toString();
    if (id.isEmpty() || !envelope.value("content").isObject() || isPending(id) || isCompleted(id))
        return fail(error, QStringLiteral("The command is already in the outbox."));
    if (pending_.size() >= maximumPending)
        return fail(error, QStringLiteral("Too many commands are waiting for this machine."));
    pending_.append({route, envelope, QDateTime::currentMSecsSinceEpoch()});
    if (save(error))
        return true;
    pending_.removeLast();
    return false;
}

bool CommandOutbox::withdraw(const QString& id, QString* error) {
    const auto before = pending_;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&id](const Entry& entry) { return entry.id() == id; }),
                   pending_.end());
    if (pending_.size() == before.size() || save(error))
        return true;
    pending_ = before;
    return false;
}

bool CommandOutbox::replace(const QString& id, Route route, const QJsonObject& envelope,
                            QString* error) {
    const auto next = envelope.value("id").toString();
    if (!isPending(id) || next.isEmpty() || !envelope.value("content").isObject() ||
        isPending(next) || isCompleted(next))
        return fail(error, QStringLiteral("Unable to persist command cleanup."));
    const auto pendingBefore = pending_;
    const auto completedBefore = completed_;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&id](const Entry& entry) { return entry.id() == id; }),
                   pending_.end());
    pending_.append({route, envelope, QDateTime::currentMSecsSinceEpoch()});
    completed_.append(id);
    while (completed_.size() > maximumCompleted)
        completed_.removeFirst();
    if (save(error))
        return true;
    pending_ = pendingBefore;
    completed_ = completedBefore;
    return false;
}

bool CommandOutbox::complete(const QString& id, QString* error) {
    const auto before = pending_;
    const auto completedBefore = completed_;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&id](const Entry& entry) { return entry.id() == id; }),
                   pending_.end());
    if (!completed_.contains(id))
        completed_.append(id);
    while (completed_.size() > maximumCompleted)
        completed_.removeFirst();
    if (save(error))
        return true;
    pending_ = before;
    completed_ = completedBefore;
    return false;
}

bool CommandOutbox::save(QString* error) {
    QJsonArray pending;
    for (const auto& entry : std::as_const(pending_))
        pending.append(QJsonObject{{"route", routeName(entry.route)},
                                   {"envelope", entry.envelope},
                                   {"createdAt", entry.createdAt}});
    QJsonArray completed;
    for (const auto& id : std::as_const(completed_))
        completed.append(id);
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1}, {"pending", pending}, {"completed", completed}})
            .toJson(QJsonDocument::Compact);
    const QFileInfo info(file_);
    if (!QDir().mkpath(info.absolutePath()) ||
        !QFile::setPermissions(info.absolutePath(),
                               QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return fail(error, QStringLiteral("Unable to create the command outbox directory."));
    QSaveFile out(file_);
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        out.write(bytes) != bytes.size() || !out.commit())
        return fail(error, QStringLiteral("Unable to write the command outbox."));
    return true;
}

} // namespace ketplus::remote
