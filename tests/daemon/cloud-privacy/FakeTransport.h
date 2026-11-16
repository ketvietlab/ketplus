#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <cstring>

namespace r03 {
class Reply final : public QNetworkReply {
    QByteArray body_;
    qint64 offset_{0};
    NetworkError failure_;

  public:
    Reply(const QNetworkRequest& request, int status, QByteArray body, QObject* parent,
          NetworkError failure = NoError, bool held = false)
        : QNetworkReply(parent), body_(std::move(body)), failure_(failure) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (!held)
            QTimer::singleShot(0, this, &Reply::finish);
    }
    void finish() {
        if (isFinished())
            return;
        if (failure_ != NoError) {
            // Like Qt after a transfer timeout: the header arrived, then the reply was
            // aborted and closed with its body unread.
            setError(failure_, QStringLiteral("synthetic failure"));
            close();
        } else {
            emit readyRead();
        }
        setFinished(true);
        emit finished();
    }
    void abort() override { setError(OperationCanceledError, "synthetic abort"); }
    qint64 bytesAvailable() const override {
        return body_.size() - offset_ + QIODevice::bytesAvailable();
    }

  protected:
    qint64 readData(char* data, qint64 maximum) override {
        const auto count = qMin(maximum, body_.size() - offset_);
        if (count <= 0)
            return -1;
        std::memcpy(data, body_.constData() + offset_, count);
        offset_ += count;
        return count;
    }
};
class Transport final : public QNetworkAccessManager {
  public:
    QSet<QString> heldPaths;
    QList<QPointer<Reply>> heldReplies;
    void release(const QString& path) {
        const auto pending = heldReplies;
        for (const auto& reply : pending) {
            if (reply && reply->url().path() == path) {
                heldReplies.removeAll(reply);
                reply->finish();
            }
        }
    }
    QList<QByteArray> bodies;
    QList<QNetworkRequest> requests;
    int status{503};
    QByteArray response{"{}"};
    QHash<QString, int> statusForPath;
    QHash<QString, QByteArray> responseForPath;
    QHash<QString, QNetworkReply::NetworkError> failureForPath;

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        const QByteArray verb =
            request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        const QString path = request.url().path();
        // The daemon polls for its next command as soon as it acknowledges one.
        const bool keyRead =
            verb == "GET" &&
            (path.startsWith(QStringLiteral("/v2/device-keys/")) ||
             path == QStringLiteral("/v2/commands:poll") || path == QStringLiteral("/v2/devices") ||
             path.startsWith(QStringLiteral("/v2/pairing/tickets/")));
        if (operation != CustomOperation ||
            (!keyRead && (verb != "POST" || (!path.endsWith(QStringLiteral("/responses:append")) &&
                                             !path.endsWith(QStringLiteral("/ack")) &&
                                             !path.endsWith(QStringLiteral("/approve")) &&
                                             !path.endsWith(QStringLiteral("/revoke")) &&
                                             path != QStringLiteral("/v2/session-keys:append") &&
                                             path != QStringLiteral("/v2/pairing/tickets") &&
                                             path != QStringLiteral("/v2/machines:enroll") &&
                                             path != QStringLiteral("/v2/events:append") &&
                                             path != QStringLiteral("/v2/daemon/challenges") &&
                                             path != QStringLiteral("/v2/daemon/credentials") &&
                                             path != QStringLiteral("/v2/daemon/heartbeat")))))
            qFatal("Unexpected request; fake transport never opens a socket");
        requests.append(request);
        bodies.append(outgoing ? outgoing->readAll() : QByteArray{});
        auto* reply = new Reply(request, statusForPath.value(request.url().path(), status),
                                responseForPath.value(request.url().path(), response), this,
                                failureForPath.value(request.url().path(), QNetworkReply::NoError),
                                heldPaths.contains(path));
        if (heldPaths.contains(path))
            heldReplies.append(reply);
        return reply;
    }
};
} // namespace r03
