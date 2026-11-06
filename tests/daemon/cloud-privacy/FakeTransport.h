#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <cstring>

namespace r03 {
class Reply final : public QNetworkReply {
    QByteArray body_;
    qint64 offset_{0};

  public:
    Reply(const QNetworkRequest& request, int status, QByteArray body, QObject* parent,
          NetworkError failure = NoError)
        : QNetworkReply(parent), body_(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this, failure] {
            if (failure != NoError) {
                // Like Qt after a transfer timeout: the header arrived, then the reply was
                // aborted and closed with its body unread.
                setError(failure, QStringLiteral("synthetic failure"));
                close();
            } else {
                emit readyRead();
            }
            setFinished(true);
            emit finished();
        });
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
        const QByteArray verb = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        const QString path = request.url().path();
        // The daemon polls for its next command as soon as it acknowledges one.
        const bool keyRead =
            verb == "GET" &&
            (path.startsWith(QStringLiteral("/v2/device-keys/")) ||
             path == QStringLiteral("/v2/commands:poll") || path == QStringLiteral("/v2/devices"));
        if (operation != CustomOperation ||
            (!keyRead && (verb != "POST" || (!path.endsWith(QStringLiteral("/responses:append")) &&
                                             !path.endsWith(QStringLiteral("/ack")) &&
                                             path != QStringLiteral("/v2/events:append") &&
                                             path != QStringLiteral("/v2/daemon/challenges") &&
                                             path != QStringLiteral("/v2/daemon/credentials") &&
                                             path != QStringLiteral("/v2/daemon/heartbeat")))))
            qFatal("Unexpected request; fake transport never opens a socket");
        requests.append(request);
        bodies.append(outgoing ? outgoing->readAll() : QByteArray{});
        return new Reply(request, statusForPath.value(request.url().path(), status),
                         responseForPath.value(request.url().path(), response), this,
                         failureForPath.value(request.url().path(), QNetworkReply::NoError));
    }
};
} // namespace r03
