#pragma once

#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <functional>

namespace ketplus::daemon::transport {
// Content-free notices that an event worth a phone notification was appended, sent on the
// daemon's message socket while events are forwarded instead of uploaded. A notice holds exactly
// five keys: type "push.signal", sessionId, routingId (the session's processInstanceId, which is
// its id), kind and cursor. It never carries text, ciphertext, titles or tool names.
// Best effort: at most maxPending notices wait for the socket, a new one replaces the oldest,
// a notice older than maxAgeMs is dropped, and nothing blocks the daemon thread.
class PushSignals final : public QObject {
    Q_OBJECT
  public:
    static constexpr int maxPending = 64;
    static constexpr qint64 maxAgeMs = 10 * 60'000;
    // False when the socket cannot take the message now.
    using Sender = std::function<bool(const QJsonObject&)>;
    using Enabled = std::function<bool()>;
    using Clock = std::function<qint64()>;

    PushSignals(Sender sender, Enabled enabled, QObject* parent = nullptr);
    void setClock(Clock clock) { clock_ = std::move(clock); }
    // The kinds of event that raise a notification; nothing else is ever signalled.
    static bool signalKind(const QString& eventType);
    // The notice for an appended journal event, or an empty object when it raises none.
    static QJsonObject message(const QJsonObject& event);
    void note(const QJsonObject& event);
    // The socket opened or may take messages again.
    void flush();
    int pending() const { return int(queue_.size()); }

  private:
    struct Entry {
        QJsonObject message;
        qint64 at{0};
    };
    QQueue<Entry> queue_;
    Sender sender_;
    Enabled enabled_;
    Clock clock_;
    QTimer retry_;
};
} // namespace ketplus::daemon::transport
