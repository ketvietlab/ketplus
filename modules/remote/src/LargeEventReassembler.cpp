#include "LargeEventReassembler.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace ketplus::remote {
namespace {

constexpr qint64 maximumLargeEventBytes = 32 * 1024 * 1024;
// The daemon sends whole journal chunks (96 KiB), so a transfer has a few hundred fragments.
// One bookkeeping entry per fragment stays bounded even if a sender splits it into bytes.
constexpr qsizetype maximumFragments = 4096;

QString itemIdOf(const QJsonObject& payload) {
    const QString direct = payload.value(QStringLiteral("item_id")).toString();
    return direct.isEmpty() ? payload.value(QStringLiteral("item"))
                                  .toObject()
                                  .value(QStringLiteral("id"))
                                  .toString()
                            : direct;
}

} // namespace

LargeEventReassembler::LargeEventReassembler(const QJsonObject& reference)
    : transferId_(reference.value(QStringLiteral("transfer_id")).toString()),
      eventType_(reference.value(QStringLiteral("event_type")).toString()),
      itemId_(reference.value(QStringLiteral("item_id")).toString()),
      digestSha256_(reference.value(QStringLiteral("digest_sha256")).toString()),
      totalBytes_(reference.value(QStringLiteral("total_bytes")).toInteger(-1)),
      revision_(reference.value(QStringLiteral("revision")).toInteger(0)) {
    valid_ =
        reference.value(QStringLiteral("version")).toInt() == 1 &&
        reference.value(QStringLiteral("encoding")).toString() == QStringLiteral("json-utf8") &&
        !transferId_.isEmpty() && transferId_.size() <= 128 && !eventType_.isEmpty() &&
        !itemId_.isEmpty() && digestSha256_.size() == 64 && totalBytes_ > 0 &&
        totalBytes_ <= maximumLargeEventBytes && revision_ > 0;
    if (valid_) {
        bytes_.reserve(totalBytes_);
    }
}

bool LargeEventReassembler::isValid() const { return valid_; }
bool LargeEventReassembler::isComplete() const { return complete_; }
qint64 LargeEventReassembler::nextOffset() const { return nextOffset_; }
QString LargeEventReassembler::transferId() const { return transferId_; }
QString LargeEventReassembler::eventType() const { return eventType_; }
QString LargeEventReassembler::itemId() const { return itemId_; }
QString LargeEventReassembler::digestSha256() const { return digestSha256_; }
qint64 LargeEventReassembler::totalBytes() const { return totalBytes_; }
qint64 LargeEventReassembler::revision() const { return revision_; }

bool LargeEventReassembler::accept(const QJsonObject& fragment, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (!valid_ || complete_) {
        return fail(QStringLiteral("Large-event transfer is not accepting fragments."));
    }
    const qint64 offset = fragment.value(QStringLiteral("offset")).toInteger(-1);
    const qint64 next = fragment.value(QStringLiteral("next_offset")).toInteger(-1);
    const QByteArray data =
        QByteArray::fromBase64(fragment.value(QStringLiteral("data")).toString().toLatin1(),
                               QByteArray::Base64UrlEncoding);
    const bool terminal = fragment.value(QStringLiteral("complete")).toBool();
    if (fragment.value(QStringLiteral("large_event_fragment_version")).toInt() != 1 ||
        fragment.value(QStringLiteral("transfer_id")).toString() != transferId_ ||
        fragment.value(QStringLiteral("event_type")).toString() != eventType_ ||
        fragment.value(QStringLiteral("item_id")).toString() != itemId_ ||
        fragment.value(QStringLiteral("revision")).toInteger() != revision_ ||
        fragment.value(QStringLiteral("digest_sha256")).toString() != digestSha256_ ||
        fragment.value(QStringLiteral("total_bytes")).toInteger() != totalBytes_ ||
        data.isEmpty() || next != offset + data.size() || next > totalBytes_ ||
        terminal != (next == totalBytes_)) {
        return fail(QStringLiteral("Conflicting large-event fragment metadata."));
    }
    const auto duplicate = accepted_.constFind(offset);
    if (duplicate != accepted_.constEnd()) {
        if (duplicate.value() != data.size() ||
            QByteArrayView(bytes_).sliced(offset, data.size()) != data) {
            return fail(QStringLiteral("Conflicting duplicate large-event fragment."));
        }
        return true;
    }
    if (offset != nextOffset_) {
        return fail(QStringLiteral("Large-event fragment gap or reordering."));
    }
    if (accepted_.size() >= maximumFragments) {
        return fail(QStringLiteral("Large-event transfer has too many fragments."));
    }
    accepted_.insert(offset, data.size());
    bytes_.append(data);
    nextOffset_ = next;
    complete_ = terminal;
    return true;
}

QJsonObject LargeEventReassembler::payload(QString* error) const {
    const auto fail = [&](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return QJsonObject{};
    };
    if (!valid_ || !complete_ || bytes_.size() != totalBytes_) {
        return fail(QStringLiteral("Large-event content is incomplete."));
    }
    const QString digest =
        QString::fromLatin1(QCryptographicHash::hash(bytes_, QCryptographicHash::Sha256).toHex());
    if (digest != digestSha256_) {
        return fail(QStringLiteral("Large-event content digest mismatch."));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes_, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        itemIdOf(document.object()) != itemId_) {
        return fail(QStringLiteral("Reassembled large-event payload is invalid."));
    }
    return document.object();
}

} // namespace ketplus::remote
