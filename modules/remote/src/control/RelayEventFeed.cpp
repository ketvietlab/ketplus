#include "control/RelayEventFeed.h"

#include "cloud/CredentialStore.h"
#include "control/RelayFrames.h"

#include <QCryptographicHash>
#include <QJsonObject>

namespace ketplus::remote {
namespace {
QString cursorAccount(const QString& sessionId, const qint64 generation) {
    const auto digest =
        QCryptographicHash::hash(QStringLiteral("%1:%2").arg(sessionId).arg(generation).toUtf8(),
                                 QCryptographicHash::Sha256);
    return QStringLiteral("event-cursor-") + QString::fromLatin1(digest.toHex().left(32));
}
} // namespace

qint64 CredentialEventCursors::load(const QString& sessionId, const qint64 generation) const {
    return qMax<qint64>(0, store_->read(cursorAccount(sessionId, generation)).toLongLong());
}

bool CredentialEventCursors::save(const QList<Entry>& entries) {
    return store_->batch([&entries](daemon::CredentialStore& view, QString* error) {
        for (const Entry& entry : entries) {
            const QString account = cursorAccount(entry.sessionId, entry.generation);
            if (!view.write(account, QByteArray::number(entry.cursor), error))
                return false;
        }
        return true;
    });
}

QByteArray RelayEventFeed::subscribe(const QString& sessionId, const Followed& state) const {
    return frames::encodeSubscribe(sessionId, state.generation, state.cursor);
}

QByteArray RelayEventFeed::follow(const QString& sessionId, const qint64 generation) {
    if (const auto known = followed_.constFind(sessionId);
        known != followed_.cend() && known->generation == generation)
        return connected_ && !known->reloading ? subscribe(sessionId, *known) : QByteArray{};
    Followed state;
    state.generation = generation;
    state.cursor = store_->load(sessionId, generation);
    followed_.insert(sessionId, state);
    return connected_ ? subscribe(sessionId, state) : QByteArray{};
}

QByteArray RelayEventFeed::unfollow(const QString& sessionId) {
    if (!followed_.contains(sessionId))
        return {};
    flush();
    followed_.remove(sessionId);
    return connected_ ? frames::encodeUnsubscribe(sessionId) : QByteArray{};
}

QList<QByteArray> RelayEventFeed::connected() {
    connected_ = true;
    QList<QByteArray> out;
    for (auto it = followed_.cbegin(); it != followed_.cend(); ++it)
        if (!it->reloading)
            out.append(subscribe(it.key(), *it));
    return out;
}

RelayEventFeed::Result RelayEventFeed::receive(const QByteArray& bytes, QString* error) {
    Result result;
    const auto frame = frames::decode(bytes, error);
    if (!frame)
        return result;
    using frames::Type;
    if (frame->type == Type::FramesDropped) {
        // The relay dropped frames for this device: every conversation, or the one it names.
        for (auto it = followed_.cbegin(); it != followed_.cend(); ++it)
            if (!it->reloading && (frame->sessionId.isEmpty() || frame->sessionId == it.key()))
                result.send.append(subscribe(it.key(), *it));
        return result;
    }
    const auto found = followed_.find(frame->sessionId);
    if (found == followed_.end())
        return result; // A frame of a conversation no longer followed.
    Followed& state = *found;
    result.sessionId = frame->sessionId;
    if (frame->type == Type::SessionReset) {
        if (frame->generation < state.generation)
            return result;
        state.generation = frame->generation;
        state.reloading = true;
        result.reload = true;
        return result;
    }
    if (state.reloading)
        return result; // Until the window is adopted, events would mix old and new history.
    if (frame->type == Type::Gap) {
        result.send.append(subscribe(frame->sessionId, state));
        return result;
    }
    if (frame->generation != 0 && frame->generation != state.generation)
        return result;
    for (const QJsonValue& value : frame->events) {
        const qint64 cursor = value.toObject().value(QStringLiteral("cursor")).toInteger();
        if (cursor <= state.cursor)
            continue; // Late or repeated.
        state.cursor = cursor;
        ++state.unsaved;
        result.events.append(value);
    }
    result.live = frame->type == Type::Events && frame->live;
    if (frame->type == Type::Events || state.unsaved >= liveSaveInterval)
        flush();
    return result;
}

QByteArray RelayEventFeed::adoptWindow(const QString& sessionId, const qint64 generation,
                                       const qint64 snapshotCursor) {
    const auto found = followed_.find(sessionId);
    if (found == followed_.end() || snapshotCursor < 0)
        return {};
    Followed& state = *found;
    state.generation = generation;
    state.cursor = snapshotCursor;
    state.reloading = false;
    state.unsaved = 1;
    flush();
    return connected_ ? subscribe(sessionId, state) : QByteArray{};
}

qint64 RelayEventFeed::cursor(const QString& sessionId) const {
    return followed_.value(sessionId).cursor;
}

bool RelayEventFeed::reloading(const QString& sessionId) const {
    return followed_.value(sessionId).reloading;
}

bool RelayEventFeed::flush() {
    QList<EventCursorStore::Entry> dirty;
    for (auto it = followed_.cbegin(); it != followed_.cend(); ++it)
        if (it->unsaved > 0 && !it->reloading)
            dirty.append({it.key(), it->generation, it->cursor});
    if (dirty.isEmpty())
        return true;
    if (!store_->save(dirty))
        return false;
    for (const auto& entry : dirty) {
        auto& state = followed_[entry.sessionId];
        state.unsaved = 0;
    }
    return true;
}

} // namespace ketplus::remote
