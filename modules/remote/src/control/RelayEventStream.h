#pragma once

#include "control/RelayEventFeed.h"
#include "control/RelayFrameLink.h"

#include <QJsonArray>
#include <QObject>
#include <QTimer>

namespace ketplus::remote {

// The events of one machine's conversations, read from the frames its host sends instead of
// from stored events. It subscribes after each conversation's cursor whenever the link opens,
// subscribes again after a gap or a drop (waiting longer each time while drops repeat), and
// tells the caller when a conversation must reload its newest window. What it delivers is still
// the host's sealed event; the caller authenticates and decrypts it.
class RelayEventStream final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        // The wait before subscribing again after a drop; it doubles while drops repeat and
        // starts over once events arrive.
        int initialResubscribeMilliseconds{250};
        int maximumResubscribeMilliseconds{60'000};
    };
    RelayEventStream(RelayFrameLink* link, QString machineId, EventCursorStore* cursors,
                     Options options = {}, QObject* parent = nullptr);

    // Follows a conversation after the cursor of the newest window the caller just read.
    void open(const QString& sessionId, qint64 generation, qint64 cursor);
    void follow(const QString& sessionId, qint64 generation);
    // After a reload: events resume after the window's snapshot cursor.
    void adoptWindow(const QString& sessionId, qint64 generation, qint64 snapshotCursor);
    void unfollow(const QString& sessionId);
    void unfollowAll();
    // False after the relay said the host is offline, until a frame of it arrives again.
    [[nodiscard]] bool hostReachable() const { return reachable_; }
    [[nodiscard]] qint64 cursor(const QString& sessionId) const { return feed_.cursor(sessionId); }
    bool flush() { return feed_.flush(); }

  signals:
    // New sealed events of one conversation, ascending and not delivered before.
    void eventsReady(const QString& sessionId, const QJsonArray& events);
    // session.reset or an event that could not be opened: read the newest window and adopt it.
    void reloadRequested(const QString& sessionId);
    void reachabilityChanged(bool reachable);

  public slots:
    // The caller could not open an event: that conversation reloads its window.
    void eventRejected(const QString& sessionId);

  private:
    void linkOpened();
    void received(const QString& machineId, const QByteArray& frame);
    void dropped(const QString& machineId, const QString& reason);
    void send(const QList<QByteArray>& frames);
    void setReachable(bool reachable);
    void resubscribe();

    RelayFrameLink* link_;
    QString machineId_;
    RelayEventFeed feed_;
    Options options_;
    QTimer resubscribe_;
    int drops_{0};
    bool reachable_{true};
};

} // namespace ketplus::remote
