#include "agent/ActivityGroupLoader.h"
#include "agent/DaemonTranscriptProjection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

namespace ketplus {
ActivityGroupLoader::ActivityGroupLoader(Reader reader, QObject* parent)
    : QObject(parent), reader_(std::move(reader)) {}

QString ActivityGroupLoader::key(const QString& session, const QString& group) {
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{session, group}).toJson(QJsonDocument::Compact));
}

QVariantMap ActivityGroupLoader::details(const QString& session, const QString& group) const {
    const auto state = states_.value(key(session, group));
    return {{"groupItems", state.rows},
            {"loading", state.open && state.inFlight},
            {"error", state.error}};
}

void ActivityGroupLoader::setOpen(const QString& session, const QString& group, qint64 watermark,
                                  bool open, qint64 membership) {
    const auto id = key(session, group);
    auto& state = states_[id];
    state.session = session;
    state.group = group;
    state.desired = qMax(state.desired, watermark);
    // Opening reads the group afresh, already without the steps that left it.
    state.membership = qMax(state.membership, membership);
    if (state.open != open) {
        ++state.epoch;
        state.open = open;
        state.rows.clear();
        state.applied = 0;
        state.snapshot = -1;
        state.revision = -1;
        state.error.clear();
    } else if (open && !state.error.isEmpty())
        state.error.clear();
    emit changed(session, group);
    if (open)
        request(id);
}

void ActivityGroupLoader::updateSummary(const QString& session, const QString& group,
                                        qint64 watermark, qint64 membership) {
    const auto id = key(session, group);
    auto found = states_.find(id);
    if (found == states_.end())
        return;
    found->desired = qMax(found->desired, watermark);
    // A step left the group: loaded rows only ever grow, so start over. A page in flight
    // belongs to the old membership; the new epoch drops it and asks again.
    if (membership > found->membership) {
        found->membership = membership;
        ++found->epoch;
        found->rows.clear();
        found->applied = 0;
        found->snapshot = -1;
        found->revision = -1;
        emit changed(session, group);
    }
    if (found->open)
        request(id);
}

void ActivityGroupLoader::closeExcept(const QString& session) {
    const auto keys = states_.keys();
    for (const auto& id : keys) {
        const auto state = states_.value(id);
        if (state.session != session && state.open)
            setOpen(state.session, state.group, state.desired, false);
    }
}

void ActivityGroupLoader::request(const QString& id) {
    auto found = states_.find(id);
    if (found == states_.end() || !found->open || found->inFlight || !found->error.isEmpty() ||
        (found->applied >= found->desired && found->snapshot < 0))
        return;
    auto& state = *found;
    QJsonObject params{{"group_id", state.group}, {"after_cursor", state.applied}};
    if (state.snapshot >= 0) {
        params.insert("snapshot_cursor", state.snapshot);
        params.insert("projection_revision", state.revision);
    }
    const auto epoch = state.epoch;
    const auto session = state.session, group = state.group;
    state.inFlight = true;
    emit changed(session, group);
    const QPointer<ActivityGroupLoader> guard(this);
    reader_(
        session, params,
        [guard, id, epoch](const QJsonObject& page, const QString& transportError) {
            if (!guard)
                return;
            auto found = guard->states_.find(id);
            if (found == guard->states_.end())
                return;
            auto& state = *found;
            state.inFlight = false;
            if (state.epoch != epoch || !state.open) {
                // Reopened while the old request was in flight: start a fresh snapshot now.
                if (state.open)
                    QTimer::singleShot(0, guard, [guard, id] {
                        if (guard)
                            guard->request(id);
                    });
                return;
            }
            QString error = transportError;
            if (page.value("error_code") == "snapshot_changed") {
                state.applied = 0;
                state.snapshot = -1;
                state.revision = -1;
                state.rows.clear();
            } else {
                if (error.isEmpty() && !page.value("ok").toBool())
                    error = page.value("error_message").toString("Could not load group details.");
                const auto next = page.value("next_cursor").toInteger(-1);
                const auto snapshot = page.value("snapshot_cursor").toInteger(-1);
                if (error.isEmpty() &&
                    (page.value("group_id").toString() != state.group ||
                     page.value("session_id").toString() != state.session ||
                     !page.value("events").isArray() || next < state.applied || snapshot < next ||
                     (page.value("has_more").toBool() && next <= state.applied)))
                    error = "Invalid activity group page.";
                if (error.isEmpty()) {
                    state.rows = agent_message::mergeDaemonHistory(
                        state.rows, agent_message::daemonWindowMessages(page));
                    if (page.contains("group_running") && !page.value("group_running").toBool())
                        for (auto& value : state.rows) {
                            auto row = value.toMap();
                            if (row.value("running").toBool()) {
                                row.insert("running", false);
                                row.insert("status", "ended");
                            }
                            value = row;
                        }
                    state.applied = next;
                    state.snapshot = page.value("has_more").toBool() ? snapshot : -1;
                    state.revision = page.value("projection_revision").toInteger(-1);
                }
                state.error = error;
            }
            guard->changed(state.session, state.group);
            if (state.error.isEmpty())
                QTimer::singleShot(0, guard, [guard, id] {
                    if (guard)
                        guard->request(id);
                });
        });
}
} // namespace ketplus
