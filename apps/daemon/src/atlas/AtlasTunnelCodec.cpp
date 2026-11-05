#include "atlas/AtlasTunnelCodec.h"
#include <QJsonDocument>
#include <QtEndian>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ketplus::daemon {
namespace {
QByteArray aad(const QString& channel, bool daemon, quint64 sequence) {
    return "ketplus.atlas.v1\n" + channel.toUtf8() + (daemon ? "\ndaemon\n" : "\ndevice\n") +
           QByteArray::number(sequence);
}
using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
} // namespace
AtlasTunnelCodec::AtlasTunnelCodec(QByteArray key, QString channel, bool daemon)
    : key_(std::move(key)), channel_(std::move(channel)), daemon_(daemon) {}
QByteArray AtlasTunnelCodec::randomKey() {
    QByteArray key(32, Qt::Uninitialized);
    return RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), key.size()) == 1 ? key
                                                                                     : QByteArray{};
}
QByteArray AtlasTunnelCodec::seal(const AtlasTunnelFrame& frame) {
    const auto header = QJsonDocument(frame.header).toJson(QJsonDocument::Compact);
    if (key_.size() != 32 || header.size() > 4096 || frame.body.size() > 16384 ||
        sent_ == UINT64_MAX)
        return {};
    QByteArray plain(4, Qt::Uninitialized);
    qToBigEndian(quint32(header.size()), plain.data());
    plain += header;
    plain += frame.body;
    QByteArray wire(8 + 12 + plain.size() + 16, Qt::Uninitialized);
    const auto sequence = sent_ + 1;
    qToBigEndian(sequence, wire.data());
    auto* bytes = reinterpret_cast<unsigned char*>(wire.data());
    if (RAND_bytes(bytes + 8, 12) != 1)
        return {};
    Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    const auto auth = aad(channel_, daemon_, sequence);
    int length = 0, tail = 0;
    if (!ctx ||
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key_.constData()),
                           bytes + 8) != 1 ||
        EVP_EncryptUpdate(ctx.get(), nullptr, &length,
                          reinterpret_cast<const unsigned char*>(auth.constData()),
                          auth.size()) != 1 ||
        EVP_EncryptUpdate(ctx.get(), bytes + 20, &length,
                          reinterpret_cast<const unsigned char*>(plain.constData()),
                          plain.size()) != 1 ||
        EVP_EncryptFinal_ex(ctx.get(), bytes + 20 + length, &tail) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, bytes + wire.size() - 16) != 1)
        return {};
    sent_ = sequence;
    return wire;
}
bool AtlasTunnelCodec::open(const QByteArray& wire, AtlasTunnelFrame* frame) {
    if (key_.size() != 32 || wire.size() < 42 || wire.size() > 65536 || received_ == UINT64_MAX)
        return false;
    const auto sequence = qFromBigEndian<quint64>(wire.constData());
    if (sequence != received_ + 1)
        return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(wire.constData());
    QByteArray plain(wire.size() - 36, Qt::Uninitialized);
    Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    const auto auth = aad(channel_, !daemon_, sequence);
    int length = 0, tail = 0;
    if (!ctx ||
        EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key_.constData()),
                           bytes + 8) != 1 ||
        EVP_DecryptUpdate(ctx.get(), nullptr, &length,
                          reinterpret_cast<const unsigned char*>(auth.constData()),
                          auth.size()) != 1 ||
        EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &length,
                          bytes + 20, plain.size()) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                            const_cast<unsigned char*>(bytes + wire.size() - 16)) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()) + length,
                            &tail) != 1)
        return false;
    const auto size = qFromBigEndian<quint32>(plain.constData());
    if (size > 4096 || size > quint32(plain.size() - 4))
        return false;
    const auto doc = QJsonDocument::fromJson(plain.mid(4, size));
    if (!doc.isObject())
        return false;
    frame->header = doc.object();
    frame->body = plain.mid(4 + size);
    if (frame->body.size() > 16384)
        return false;
    received_ = sequence;
    return true;
}
} // namespace ketplus::daemon
