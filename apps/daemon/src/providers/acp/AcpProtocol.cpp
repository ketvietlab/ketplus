#include "providers/ProviderFrameValidation.h"
#include "providers/ProviderResponseEnvelope.h"
#include "providers/ProviderResponseId.h"
#include "providers/acp/AcpAdapter.h"
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <algorithm>
#include <utility>
namespace {
constexpr qsizetype maximumProviderMessageBytes = 16 * 1024 * 1024;

} // namespace

namespace ketplus::daemon {
QString AcpAdapter::responseError(const QJsonObject& error) {
    const QString message = error.value(QStringLiteral("message")).toString();
    if (!message.isEmpty())
        return message;
    const QJsonObject data = error.value(QStringLiteral("data")).toObject();
    const QString nested = data.value(QStringLiteral("message")).toString();
    if (!nested.isEmpty())
        return nested;
    return QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact));
}

QString AcpAdapter::requestKey(const QJsonValue& id) {
    return id.isString() ? id.toString() : QString::number(id.toVariant().toLongLong());
}

void AcpAdapter::sendCall(const QString& method, const QJsonObject& params, Callback callback) {
    const quint64 id = nextRequestId_++;
    pending_.insert(id, std::move(callback));
    pendingMethods_.insert(id, method);
    if (const int timeout = requestTimeoutMs(method); timeout > 0) {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        pendingTimers_.insert(id, timer);
        connect(timer, &QTimer::timeout, this, [this, id, method, timer] {
            pendingTimers_.remove(id);
            timer->deleteLater();
            if (!pending_.contains(id))
                return;
            const QString reason = processFailure(
                QStringLiteral("%1 ACP request %2 timed out.").arg(label_, method), std::nullopt);
            setReady(false);
            failPending(reason);
            if (!stopping_) {
                noteUnexpectedStop();
                emit stopped(reason);
                stopping_ = true;
            }
            process_->kill();
        });
        timer->start(timeout);
    }
    sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), static_cast<qint64>(id)},
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), params}});
}

void AcpAdapter::sendObject(const QJsonObject& object) {
    if (process_->state() == QProcess::NotRunning)
        return;
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process_->write(payload);
}

void AcpAdapter::consumeStdout() {
    if (malformedOutput_) {
        process_->setReadChannel(QProcess::StandardOutput);
        while (process_->bytesAvailable() > 0)
            if (process_->read(64 * 1024).isEmpty())
                break;
        return;
    }
    constexpr qint64 readChunkBytes = 64 * 1024;
    process_->setReadChannel(QProcess::StandardOutput);
    while (process_->bytesAvailable() > 0) {
        const QByteArray chunk = process_->read(readChunkBytes);
        if (chunk.isEmpty())
            return;
        stdoutBuffer_.append(chunk);
        while (true) {
            const qsizetype newline = stdoutBuffer_.indexOf('\n');
            const qsizetype lineBytes = newline < 0 ? stdoutBuffer_.size() : newline;
            if (lineBytes > maximumProviderMessageBytes) {
                const QString reason =
                    QStringLiteral("The %1 ACP process emitted an oversized message.").arg(label_);
                stdoutBuffer_.clear();
                // Quarantine this process generation, including already-buffered output.
                // Closing a read channel alone leaves its received bytes available.
                process_->closeReadChannel(QProcess::StandardOutput);
                while (process_->bytesAvailable() > 0)
                    if (process_->read(readChunkBytes).isEmpty())
                        break;
                failPending(reason);
                // A pending initialize has published this stop itself and marked it. Otherwise
                // publish it here, once: the exit that follows must not publish another or
                // count as a second crash.
                if (!stopping_) {
                    noteUnexpectedStop();
                    emit stopped(reason);
                    stopping_ = true;
                }
                process_->kill();
                return;
            }
            if (newline < 0)
                break;
            const QByteArray line = stdoutBuffer_.left(newline).trimmed();
            stdoutBuffer_.remove(0, newline + 1);
            if (!line.isEmpty())
                consumeLine(line);
            if (malformedOutput_)
                return;
        }
    }
}

void AcpAdapter::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!providerFrameHasValidControlBytes(line) || parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        // No reliable call identity survives a broken frame. Quarantine the whole
        // generation; finished settles pending calls through the existing lifecycle.
        malformedOutput_ = true;
        setReady(false);
        stdoutBuffer_.clear();
        process_->closeReadChannel(QProcess::StandardOutput);
        process_->kill();
        return;
    }
    const QJsonObject object = document.object();
    const QString method = object.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("session/update")) {
        handleSessionUpdate(object.value(QStringLiteral("params")).toObject());
        return;
    }
    if (!method.isEmpty() && object.contains(QStringLiteral("id"))) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (method == QStringLiteral("session/request_permission")) {
            handlePermissionRequest(object.value(QStringLiteral("id")), params);
            return;
        }
        if (handleExtensionRequest(object))
            return;
        emit requestReceived(object.value(QStringLiteral("id")), method, params);
        return;
    }
    if (!object.contains(QStringLiteral("id")))
        return;
    const auto responseId = providerResponseId(object.value(QStringLiteral("id")));
    if (!responseId)
        return;
    const quint64 id = *responseId;
    const auto callback = pending_.take(id);
    if (auto* timer = pendingTimers_.take(id)) {
        timer->stop();
        timer->deleteLater();
    }
    const QString requestMethod = pendingMethods_.take(id);
    if (!callback)
        return;
    if (!validProviderResponseEnvelope(object)) {
        callback({}, QStringLiteral("Malformed provider response envelope."));
    } else if (object.contains(QStringLiteral("error"))) {
        const QJsonObject error = object.value(QStringLiteral("error")).toObject();
        if (error.value(QStringLiteral("code")).toInt() == -32601)
            (void)narrowing_.methodNotFound(requestMethod);
        callback({}, responseError(error));
    } else
        callback(object.value(QStringLiteral("result")).toObject(), {});
}

void AcpAdapter::respond(const QJsonValue& requestId, const QJsonObject& result) {
    if (result.contains(QStringLiteral("outcome"))) {
        sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), requestId},
                    {QStringLiteral("result"), result}});
        return;
    }
    const auto permissionOptions = permissionOptionsByRequest_.take(requestKey(requestId));
    const bool allow =
        result.value(QStringLiteral("allow")).toBool() ||
        result.value(QStringLiteral("decision")).toString() == QStringLiteral("accept");
    QString optionId;
    for (const auto& value : permissionOptions) {
        const auto option = value.toObject();
        if (option.value("kind") == (allow ? "allow_once" : "reject_once") ||
            (option.value("kind").toString().isEmpty() &&
             option.value("optionId") == (allow ? "allow-once" : "reject-once"))) {
            optionId = option.value("optionId").toString();
            break;
        }
    }
    // Older agents omit options; retain their documented ids for compatibility.
    if (permissionOptions.isEmpty())
        optionId = allow ? "allow-once" : "reject-once";
    QJsonObject outcome{{"outcome", "cancelled"}};
    if (!optionId.isEmpty())
        outcome = {{"outcome", "selected"}, {"optionId", optionId}};
    sendObject(
        {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", QJsonObject{{"outcome", outcome}}}});
}

void AcpAdapter::respondError(const QJsonValue& requestId, const QString& code,
                              const QString& message) {
    permissionOptionsByRequest_.remove(requestKey(requestId));
    sendObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), requestId},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                      {QStringLiteral("message"), message}}}});
}

} // namespace ketplus::daemon
