#pragma once
#include "LargeEventReassembler.h"
#include "control/ControlV2Client.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

// Shared by ControlV2Restore.cpp (one sealed restore request: send, verify the receipt, poll and
// verify its host-signed fragments) and ControlV2Transcript.cpp (the window it pages and the
// continuation chain for large events).
namespace ketplus::remote {
namespace control_v2 {
inline QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace control_v2

struct ControlV2Client::RestoreState {
    QJsonObject session;
    QByteArray signing;
    QJsonObject root;
    QJsonObject current;
    QDateTime deadline;
    qint64 beforeCursor{0};
    qint64 snapshotCursor{0};
    int eventLimit{128};
    int turnLimit{3};
    QString owner;
    int byteCap{512 * 1024};
    // The current request reached the relay, so abandoning it must cancel it there.
    bool created{false};
    // Response fragments of the current request.
    qint64 responseCursor{0};
    int attempt{0};
    QHash<qint64, QJsonObject> responses;
    QByteArray bytes;
    qint64 totalBytes{-1};
    QString terminalDigest;
    // The root page and the large event being reassembled.
    QJsonObject page;
    QJsonArray events;
    qsizetype nextEvent{0};
    qsizetype largeEvent{-1};
    std::unique_ptr<LargeEventReassembler> assembler;
    quint64 epoch{0};
    bool finished{false};
    Callback done;
};
} // namespace ketplus::remote
