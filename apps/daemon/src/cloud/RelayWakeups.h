#pragma once

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>

namespace ketplus::daemon {

// The relay's wake-up socket. The relay says only *that* commands are waiting;
// the daemon still reads them through the command poll, which keeps running
// slower as the backstop. A lost or repeated wake-up costs latency, nothing else.
class RelayWakeups final : public QObject {
    Q_OBJECT

  public:
    // Answers with a current daemon credential, or an empty one when there is none.
    using CredentialSource = std::function<void(std::function<void(const QString&)>)>;

    static constexpr int closeCredentialExpired = 4001;

    RelayWakeups(const QUrl& controlBaseUrl, CredentialSource credential,
                 QObject* parent = nullptr);
    ~RelayWakeups() override;

    bool send(const QJsonObject& message);
    void start();
    void stop();
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] QUrl url() const { return url_; }

    // Tests shorten the waits.
    void setTimings(int initialRetryMilliseconds, int maximumRetryMilliseconds,
                    int pingMilliseconds);

  signals:
    // Something may be waiting for this machine: poll now.
    void wake();
    void message(const QJsonObject& message);
    void openChanged(bool open);
    // The relay closed the socket because the credential ran out or stopped being valid.
    void credentialExpired();
    void authorizationRevoked();

  private:
    void connectNow();
    void opened();
    void closed(QWebSocket* socket);
    void received(const QString& text);
    void scheduleRetry(int milliseconds);
    [[nodiscard]] int nextBackoff();

    QUrl url_;
    CredentialSource credential_;
    QWebSocket* socket_{nullptr};
    QTimer retryTimer_;
    QTimer pingTimer_;
    quint64 generation_{0};
    qint64 openedAt_{0};
    bool running_{false};
    bool open_{false};
    bool awaitingPong_{false};
    int failures_{0};
    int initialRetryMilliseconds_{1'000};
    int maximumRetryMilliseconds_{60'000};
    int pingMilliseconds_{25'000};
};

} // namespace ketplus::daemon
