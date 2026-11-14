#include "schedules/ScheduleClient.h"

#include "schedules/ScheduleRules.h"

#include "Schedule.h"

#include <QJsonArray>
#include <QPointer>

#include <algorithm>

namespace ketplus {

ScheduleClient::ScheduleClient(QObject* parent) : QObject(parent) {}

void ScheduleClient::setTransport(Transport transport) { transport_ = std::move(transport); }

void ScheduleClient::setAvailable(const bool available) {
    if (available_ == available)
        return;
    available_ = available;
    listError_.clear();
    if (!available_) {
        schedules_.clear();
        runs_.clear();
        latestRun_.clear();
        ++listRequest_;
        emit changed();
        return;
    }
    refresh();
}

void ScheduleClient::refresh() {
    if (!available_ || !transport_)
        return;
    const QPointer<ScheduleClient> alive(this);
    const int request = ++listRequest_;
    transport_(QStringLiteral("schedule.list"), {},
               [alive, request](const QJsonObject& result, const QString& error) {
                   if (!alive || !alive->available_ || request != alive->listRequest_)
                       return;
                   if (!error.isEmpty()) {
                       alive->listError_ = scheduleFailureText(result, error);
                       emit alive->changed();
                       return;
                   }
                   alive->listError_.clear();
                   QHash<QString, QJsonObject> next;
                   for (const QJsonValue value :
                        result.value(QStringLiteral("schedules")).toArray()) {
                       const QJsonObject schedule = value.toObject();
                       const QString id = schedule.value(QStringLiteral("id")).toString();
                       if (!id.isEmpty())
                           next.insert(id, schedule);
                   }
                   alive->schedules_ = next;
                   emit alive->changed();
               });
}

void ScheduleClient::store(const QJsonObject& schedule) {
    const QString id = schedule.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    if (const auto it = schedules_.constFind(id); it != schedules_.cend() && *it == schedule)
        return;
    schedules_.insert(id, schedule);
    emit changed();
}

void ScheduleClient::prependRun(const QJsonObject& run) {
    const QString scheduleId = run.value(QStringLiteral("schedule_id")).toString();
    const QString runId = run.value(QStringLiteral("id")).toString();
    if (scheduleId.isEmpty() || runId.isEmpty())
        return;
    latestRun_.insert(scheduleId, run);
    if (const auto it = runs_.find(scheduleId); it != runs_.end() && it->loaded) {
        const auto same =
            std::find_if(it->items.cbegin(), it->items.cend(), [&](const QJsonObject& item) {
                return item.value(QStringLiteral("id")).toString() == runId;
            });
        if (same == it->items.cend()) {
            it->items.prepend(run);
            while (it->items.size() > schedule::maximumRetainedRuns)
                it->items.removeLast();
            emit runsChanged(scheduleId);
        }
    }
    emit changed();
}

void ScheduleClient::applyEvent(const QString& type, const QJsonObject& payload) {
    if (!available_)
        return;
    if (type == QLatin1String("schedule.changed")) {
        const QString change = payload.value(QStringLiteral("change")).toString();
        const QJsonObject schedule = payload.value(QStringLiteral("schedule")).toObject();
        const QString id = schedule.value(QStringLiteral("id")).toString();
        if (change == QLatin1String("deleted")) {
            if (schedules_.remove(id) > 0) {
                runs_.remove(id);
                latestRun_.remove(id);
                emit changed();
            }
            return;
        }
        if (change == QLatin1String("created") || change == QLatin1String("updated"))
            store(schedule);
        return;
    }
    if (type == QLatin1String("schedule.run"))
        prependRun(payload.value(QStringLiteral("run")).toObject());
}

void ScheduleClient::absorb(const QString& type, const QJsonObject& payload,
                            const QJsonObject& result) {
    if (result.contains(QStringLiteral("schedule")))
        store(result.value(QStringLiteral("schedule")).toObject());
    if (type == QLatin1String("schedule.delete")) {
        const QString id = result.value(QStringLiteral("id"))
                               .toString(payload.value(QStringLiteral("id")).toString());
        if (schedules_.remove(id) > 0) {
            runs_.remove(id);
            latestRun_.remove(id);
            emit changed();
        }
    }
    if (type == QLatin1String("schedule.runs")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        RunPage& page = runs_[id];
        QList<QJsonObject> received;
        for (const QJsonValue value : result.value(QStringLiteral("runs")).toArray())
            received.append(value.toObject());
        if (payload.contains(QStringLiteral("before_id")))
            page.items.append(received);
        else
            page.items = received;
        page.loaded = true;
        page.hasMore = result.value(QStringLiteral("has_more")).toBool();
        if (!page.items.isEmpty())
            latestRun_.insert(id, page.items.constFirst());
        emit runsChanged(id);
        emit changed();
    }
    if (type == QLatin1String("schedule.runNow") && result.contains(QStringLiteral("run")))
        prependRun(result.value(QStringLiteral("run")).toObject());
}

void ScheduleClient::request(const QString& type, const QJsonObject& payload, Reply reply) {
    if (!available_ || !transport_) {
        if (reply)
            reply({}, QStringLiteral("Schedules need a KetPlus Daemon that supports them."));
        return;
    }
    const QPointer<ScheduleClient> alive(this);
    transport_(type, payload,
               [alive, type, payload, reply = std::move(reply)](const QJsonObject& result,
                                                                const QString& error) {
                   if (alive && error.isEmpty())
                       alive->absorb(type, payload, result);
                   if (reply)
                       reply(result, error);
               });
}

void ScheduleClient::loadRuns(const QString& id, const bool more) {
    QJsonObject payload{{QStringLiteral("id"), id}, {QStringLiteral("limit"), runsPageSize}};
    if (more) {
        const RunPage page = runs_.value(id);
        if (!page.loaded || !page.hasMore || page.items.isEmpty())
            return;
        payload.insert(QStringLiteral("before_id"),
                       page.items.constLast().value(QStringLiteral("id")).toString());
    }
    request(QStringLiteral("schedule.runs"), payload);
}

QList<QJsonObject> ScheduleClient::schedules() const {
    QList<QJsonObject> list = schedules_.values();
    std::sort(list.begin(), list.end(), [](const QJsonObject& left, const QJsonObject& right) {
        const QString leftName = left.value(QStringLiteral("name")).toString();
        const QString rightName = right.value(QStringLiteral("name")).toString();
        if (leftName != rightName)
            return leftName.localeAwareCompare(rightName) < 0;
        return left.value(QStringLiteral("id")).toString() <
               right.value(QStringLiteral("id")).toString();
    });
    return list;
}

std::optional<QJsonObject> ScheduleClient::schedule(const QString& id) const {
    const auto it = schedules_.constFind(id);
    if (it == schedules_.cend())
        return std::nullopt;
    return *it;
}

QList<QJsonObject> ScheduleClient::runs(const QString& id) const { return runs_.value(id).items; }

bool ScheduleClient::runsLoaded(const QString& id) const { return runs_.value(id).loaded; }

bool ScheduleClient::runsHaveMore(const QString& id) const { return runs_.value(id).hasMore; }

QJsonObject ScheduleClient::latestRun(const QString& id) const { return latestRun_.value(id); }

} // namespace ketplus
