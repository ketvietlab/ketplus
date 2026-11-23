#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>

namespace ketplus::daemon {
class CredentialStore;
}

namespace ketplus::remote {

// Where a conversation's event cursor survives a restart.
class EventCursorStore {
  public:
    struct Entry {
        QString sessionId;
        qint64 generation{0};
        qint64 cursor{0};
    };
    virtual ~EventCursorStore() = default;
    // 0 when nothing is stored.
    [[nodiscard]] virtual qint64 load(const QString& sessionId, qint64 generation) const = 0;
    // Every entry or none.
    virtual bool save(const QList<Entry>& entries) = 0;
};

// Cursors in the desktop's encrypted credential store, used when no local encrypted cache holds
// them. All cursors live in one record keyed by a digest of the conversation id and generation,
// so the store lists no conversation ids. The record is read once, on the first load, and every
// save is one write for all cursors that changed: the number of store accesses does not grow
// with the number of followed conversations.
class CredentialEventCursors final : public EventCursorStore {
  public:
    explicit CredentialEventCursors(daemon::CredentialStore* store) : store_(store) {}
    [[nodiscard]] qint64 load(const QString& sessionId, qint64 generation) const override;
    bool save(const QList<Entry>& entries) override;
    // Reads of the store so far, for tests that bound its use.
    [[nodiscard]] int storeReads() const { return reads_; }

  private:
    void ensureLoaded() const;

    daemon::CredentialStore* store_;
    mutable bool loaded_{false};
    mutable int reads_{0};
    mutable QHash<QString, qint64> cursors_;
};

// The frames of followed conversations as one ordered stream: it tracks a cursor per
// conversation, delivers each event once, in order, and says which frame to send next after a
// gap, a drop or a reset. It owns no socket; the caller sends what it returns and reports
// when a connection opens. Not thread safe.
class RelayEventFeed final {
  public:
    // A cursor is saved after each page and after this many live events in between.
    static constexpr int liveSaveInterval = 50;

    explicit RelayEventFeed(EventCursorStore* store) : store_(store) {}
    ~RelayEventFeed() { flush(); }

    struct Result {
        // New events of one conversation, ascending and not delivered before.
        QString sessionId;
        QJsonArray events;
        // Frames to send, in order.
        QList<QByteArray> send;
        // The conversation must reload its newest window; call adoptWindow() with its snapshot.
        bool reload{false};
        // The page was the host's last before live events.
        bool live{false};
    };

    // Starts following from the stored cursor; the frame to send, empty without a connection.
    QByteArray follow(const QString& sessionId, qint64 generation);
    QByteArray unfollow(const QString& sessionId);
    // A connection is open: one subscribe after each cursor.
    QList<QByteArray> connected();
    // The connection is lost; frames arrive again only after connected().
    void disconnected() { connected_ = false; }

    // One received frame. A rejected frame changes nothing and sets `error`.
    Result receive(const QByteArray& bytes, QString* error = nullptr);

    // After a reset: the newest window was read at this cursor. Events resume after it.
    QByteArray adoptWindow(const QString& sessionId, qint64 generation, qint64 snapshotCursor);

    [[nodiscard]] qint64 cursor(const QString& sessionId) const;
    [[nodiscard]] bool reloading(const QString& sessionId) const;
    bool flush();

  private:
    struct Followed {
        qint64 generation{0};
        qint64 cursor{0};
        int unsaved{0};
        bool reloading{false};
    };
    [[nodiscard]] QByteArray subscribe(const QString& sessionId, const Followed& state) const;

    EventCursorStore* store_;
    QHash<QString, Followed> followed_;
    bool connected_{false};
};

} // namespace ketplus::remote
