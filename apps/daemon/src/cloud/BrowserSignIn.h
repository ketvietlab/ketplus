#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;

namespace ketplus::daemon {

// One sign-in through the system browser (RFC 8252 with PKCE S256). The relay's sign-in page
// sends the browser back to a loopback port this listens on, with a one-time code and the
// state it was given; only this object holds the verifier that redeems that code.
class BrowserSignIn final : public QObject {
    Q_OBJECT

  public:
    // The relay forgets a handoff after ten minutes; waiting longer is pointless.
    static constexpr int expiryMilliseconds = 10 * 60'000;

    explicit BrowserSignIn(QObject* parent = nullptr);

    // Listens on 127.0.0.1 at a port the system chooses.
    bool listen(QString* error);
    [[nodiscard]] QString redirectUri() const;
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString codeChallenge() const;
    [[nodiscard]] QString codeVerifier() const { return verifier_; }

  signals:
    // Emitted once; the listener is closed by then.
    void codeReceived(const QString& code);
    void failed(const QString& error);

  private:
    void read(QTcpSocket* socket);
    void respond(QTcpSocket* socket, QByteArray status, const QString& message);

    QTcpServer server_;
    QTimer expiry_;
    QString state_;
    QString verifier_;
    quint16 port_{0};
    bool finished_{false};
};

} // namespace ketplus::daemon
