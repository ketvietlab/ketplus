#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

namespace ketplus::remote {

// One text connection to the relay, as the link needs it; the Qt WebSocket in production and a
// scripted peer in tests.
class FrameSocket : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void open(const QUrl& url, const QString& bearer) = 0;
    virtual bool send(const QByteArray& text) = 0;
    virtual void close() = 0;
  signals:
    void opened();
    void closed(int code);
    void text(const QByteArray& message);
};

// The device's socket to the relay, kept open with backoff, carrying event frames of every
// machine this account reads. It routes a received frame to its machine and says when the
// relay dropped frames; what a frame means belongs to RelayEventStream.
class RelayFrameLink final : public QObject {
    Q_OBJECT
  public:
    enum class State { Stopped, Connecting, Open, Waiting };
    Q_ENUM(State)
    struct Options {
        int initialBackoffMilliseconds{1000};
        int maximumBackoffMilliseconds{60'000};
        // A connection that stayed open this long starts the backoff over.
        int healthyMilliseconds{30'000};
    };
    using TokenSource = std::function<void(std::function<void(const QString&, const QString&)>)>;
    using SocketFactory = std::function<FrameSocket*(QObject* parent)>;
    // The largest frame the link sends (the relay refuses more) and accepts.
    static constexpr qsizetype maximumSendBytes = 4 * 1024;
    static constexpr qsizetype maximumReceiveBytes = 66 * 1024;

    // Overloads rather than a default argument: a nested type's member initializers are not
    // usable in a default argument inside the enclosing class (Apple clang refuses it).
    RelayFrameLink(const QUrl& origin, TokenSource token, SocketFactory factory, Options options,
                   QObject* parent = nullptr);
    RelayFrameLink(const QUrl& origin, TokenSource token, SocketFactory factory,
                   QObject* parent = nullptr);
    ~RelayFrameLink() override;

    void start();
    void stop();
    [[nodiscard]] State state() const { return state_; }
    // Wraps one frame for a machine; false when the link is not open or the frame is too large.
    bool send(const QString& machineId, const QByteArray& frame);
    // The production socket.
    [[nodiscard]] static SocketFactory qtSockets();

  signals:
    void stateChanged(ketplus::remote::RelayFrameLink::State state);
    // The socket opened: subscriptions are gone and must be sent again.
    void opened();
    void frameReceived(const QString& machineId, const QByteArray& frame);
    void dropped(const QString& machineId, const QString& reason);
    // The relay closed the socket because this device is no longer paired.
    void authorizationRevoked();

  private:
    void connectNow();
    void socketClosed(int code);
    void setState(State state);
    void scheduleRetry();
    void received(const QByteArray& message);

    QUrl url_;
    TokenSource token_;
    SocketFactory factory_;
    Options options_;
    FrameSocket* socket_{nullptr};
    State state_{State::Stopped};
    QTimer retry_;
    quint64 attempt_{0};
    int failures_{0};
    qint64 openedAt_{0};
};

} // namespace ketplus::remote
