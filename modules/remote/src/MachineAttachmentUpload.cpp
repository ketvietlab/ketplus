#include "MachineAttachmentUpload.h"
#include <QCryptographicHash>
#include <QTimer>
#include <QUuid>

namespace ketplus::remote {
namespace {
constexpr int chunkBytes = 32 * 1024;
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace
MachineAttachmentUpload::MachineAttachmentUpload(MachineBackend* backend, QString sessionId,
                                                 qint64 generation, QObject* parent)
    : QObject(parent), backend_(backend), sessionId_(std::move(sessionId)),
      id_(QUuid::createUuid().toString(QUuid::WithoutBraces)), generation_(generation) {
    connect(backend, &MachineBackend::commandCompleted, this,
            [this](const QString& command, const QJsonObject& result, const QString& error) {
                if (!queuedCommand_.isEmpty() && command == queuedCommand_) {
                    queuedCommand_.clear();
                    accept(result, error);
                }
            });
    connect(backend, &QObject::destroyed, this, [this] { fail(tr("The machine disconnected.")); });
}
void MachineAttachmentUpload::start(QString name, QString mediaType, QByteArray bytes) {
    if (started_)
        return;
    started_ = true;
    name_ = std::move(name);
    mediaType_ = std::move(mediaType);
    bytes_ = std::move(bytes);
    if (bytes_.isEmpty() || bytes_.size() > 20 * 1024 * 1024 ||
        (mediaType_ == QLatin1String("text/plain") &&
         (bytes_.size() > 512 * 1024 || bytes_.contains('\0'))) ||
        (mediaType_ != QLatin1String("text/plain") && mediaType_ != QLatin1String("image/png") &&
         mediaType_ != QLatin1String("image/jpeg"))) {
        fail(tr("Choose an image up to 20 MB or a text file up to 512 KB."));
        return;
    }
    advance();
}
void MachineAttachmentUpload::cancel() {
    if (!started_ || (done_ && !ready_))
        return;
    cancelling_ = true;
    ready_ = false;
    done_ = false;
    if (!pending_)
        advance();
}
void MachineAttachmentUpload::advance() {
    if (done_ || pending_)
        return;
    if (!backend_) {
        fail(tr("The machine disconnected."));
        return;
    }
    using Operation = MachineBackend::AttachmentOperation;
    Operation operation;
    QJsonObject request{{"attachment_id", id_}, {"expected_revision", step_ < 0 ? 0 : 1}};
    if (cancelling_) {
        operation = Operation::Cancel;
    } else if (step_ < 0) {
        operation = Operation::Begin;
        request.insert("name", name_);
        request.insert("media_type", mediaType_);
        request.insert("total_bytes", bytes_.size());
        request.insert("chunk_count", (bytes_.size() + chunkBytes - 1) / chunkBytes);
        request.insert("digest_sha256", digest(bytes_));
    } else if (chunk_ * chunkBytes < bytes_.size()) {
        operation = Operation::Chunk;
        const auto data = bytes_.mid(chunk_ * chunkBytes, chunkBytes);
        request.insert("chunk_index", chunk_);
        request.insert("data", QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding |
                                                                 QByteArray::OmitTrailingEquals)));
        request.insert("digest_sha256", digest(data));
    } else {
        operation = Operation::Commit;
    }
    sentCancel_ = cancelling_;
    pending_ = true;
    const QPointer<MachineAttachmentUpload> guard(this);
    backend_->attachmentCommand(
        operation, sessionId_, generation_, request,
        [guard](const QJsonObject& result, const QString& error) {
            if (!guard)
                return;
            if (error.isEmpty() && result.value("status").toString() == QLatin1String("queued")) {
                guard->queuedCommand_ = result.value("command_id").toString();
                if (guard->queuedCommand_.isEmpty())
                    guard->fail(tr("The upload command has no receipt."));
                return;
            }
            guard->accept(result, error);
        });
}
void MachineAttachmentUpload::accept(const QJsonObject& result, const QString& error) {
    if (done_)
        return;
    pending_ = false;
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    if (result.contains("ok") && !result.value("ok").toBool()) {
        fail(result.value("error_message").toString(tr("The attachment could not be staged.")));
        return;
    }
    if (sentCancel_) {
        done_ = true;
        bytes_.clear();
        emit finished({}, tr("Attachment cancelled."));
        return;
    }
    if (step_ < 0) {
        if (result.value("revision").toInteger() != 1) {
            fail(tr("The upload revision was not confirmed."));
            return;
        }
        step_ = 0;
    } else if (chunk_ * chunkBytes < bytes_.size()) {
        if (!result.value("accepted").toBool()) {
            fail(tr("The attachment chunk was rejected."));
            return;
        }
        ++chunk_;
        emit progress(qMin<qint64>(chunk_ * chunkBytes, bytes_.size()), bytes_.size());
    } else {
        if (result.value("status").toString() != QLatin1String("ready")) {
            fail(tr("The attachment is incomplete."));
            return;
        }
        ready_ = done_ = true;
        bytes_.clear();
        emit finished(id_, {});
        return;
    }
    QTimer::singleShot(0, this, &MachineAttachmentUpload::advance);
}
void MachineAttachmentUpload::fail(const QString& error) {
    if (done_)
        return;
    done_ = true;
    pending_ = false;
    queuedCommand_.clear();
    bytes_.clear();
    emit finished({}, error);
}
} // namespace ketplus::remote
