#include "cloud/BrowserSignIn.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace ketplus::daemon {
namespace {

// A browser's request line and headers; anything larger is not the callback.
constexpr qsizetype maximumRequestBytes = 8 * 1024;

QString randomToken() {
    // QRandomGenerator::system() is the OS cryptographically secure source.
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool sameSecret(const QString& left, const QString& right) {
    const QByteArray a = left.toUtf8();
    const QByteArray b = right.toUtf8();
    if (a.size() != b.size())
        return false;
    char difference = 0;
    for (qsizetype index = 0; index < a.size(); ++index)
        difference |= static_cast<char>(a.at(index) ^ b.at(index));
    return difference == 0;
}

} // namespace

BrowserSignIn::BrowserSignIn(QObject* parent)
    : QObject(parent), state_(randomToken()), verifier_(randomToken()) {
    expiry_.setSingleShot(true);
    expiry_.setInterval(expiryMilliseconds);
    connect(&expiry_, &QTimer::timeout, this, [this] {
        if (finished_)
            return;
        finished_ = true;
        server_.close();
        emit failed(QStringLiteral("Sign-in timed out. Try again."));
    });
    connect(&server_, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            socket->setParent(this);
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        }
    });
}

bool BrowserSignIn::listen(QString* error) {
    // Loopback only: the code must never reach another host on the network.
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
        if (error != nullptr)
            *error = QStringLiteral("Unable to receive the browser sign-in: %1")
                         .arg(server_.errorString());
        return false;
    }
    // Kept past close(): the code is redeemed with this redirect after the listener stops.
    port_ = server_.serverPort();
    expiry_.start();
    return true;
}

QString BrowserSignIn::redirectUri() const {
    return QStringLiteral("http://127.0.0.1:%1/callback").arg(port_);
}

QString BrowserSignIn::codeChallenge() const {
    return QString::fromLatin1(
        QCryptographicHash::hash(verifier_.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

void BrowserSignIn::read(QTcpSocket* socket) {
    QByteArray received = socket->property("received").toByteArray() + socket->readAll();
    if (received.size() > maximumRequestBytes) {
        socket->abort();
        return;
    }
    if (!received.contains("\r\n\r\n")) {
        socket->setProperty("received", received);
        return;
    }
    const QList<QByteArray> requestLine = received.left(received.indexOf("\r\n")).split(' ');
    const QUrl target =
        requestLine.size() == 3
            ? QUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(requestLine.at(1)))
            : QUrl();
    if (requestLine.value(0) != "GET" || target.path() != QStringLiteral("/callback")) {
        // Browsers also ask for /favicon.ico; nothing but the callback is answered.
        respond(socket, "404 Not Found", QStringLiteral("Not found."));
        return;
    }
    if (finished_) {
        respond(socket, "410 Gone", QStringLiteral("This sign-in has already finished."));
        return;
    }
    const QUrlQuery query(target);
    const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    static const QRegularExpression codePattern(QStringLiteral("^[A-Za-z0-9._~-]{16,256}$"));
    // A page that does not know the state cannot finish or spoil this sign-in.
    const bool ownState =
        sameSecret(query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded), state_);
    // The relay's answer to a declined or failed sign-in ends it now, not at the timeout. Its
    // error_description is never shown (#84).
    if (ownState && query.hasQueryItem(QStringLiteral("error"))) {
        const bool declined =
            query.queryItemValue(QStringLiteral("error")) == QStringLiteral("access_denied");
        finished_ = true;
        expiry_.stop();
        server_.close();
        respond(socket, "200 OK",
                declined ? QStringLiteral("Sign-in was declined. You can close this tab.")
                         : QStringLiteral("Sign-in did not finish. Return to KetPlus to try "
                                          "again."));
        emit failed(declined ? QStringLiteral("Sign-in was declined.")
                             : QStringLiteral("Sign-in did not finish. Try again."));
        return;
    }
    if (!ownState || !codePattern.match(code).hasMatch()) {
        respond(socket, "400 Bad Request",
                QStringLiteral("This sign-in link is not for this KetPlus."));
        return;
    }
    finished_ = true;
    expiry_.stop();
    server_.close();
    respond(socket, "200 OK",
            QStringLiteral("You are signed in. Return to KetPlus; you can close this tab."));
    emit codeReceived(code);
}

void BrowserSignIn::respond(QTcpSocket* socket, QByteArray status, const QString& message) {
    const QByteArray body =
        QStringLiteral("<!doctype html><meta charset=\"utf-8\"><title>KetPlus</title>"
                       "<body style=\"font:16px -apple-system,system-ui,sans-serif;"
                       "margin:48px;text-align:center\"><p>%1</p></body>")
            .arg(message)
            .toUtf8();
    socket->write("HTTP/1.1 " + status +
                  "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store"
                  "\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
}

} // namespace ketplus::daemon
