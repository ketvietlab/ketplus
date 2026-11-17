#include "cloud/CloudControlClient.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>

namespace ketplus::daemon {
QJsonArray CloudControlClient::sessionKeyEnvelopes(const QJsonArray& devices,
                                                   const bool requirePins, const qsizetype limit,
                                                   QJsonArray* trusted, QString* error) {
    const auto sessions = journal_->sessions();
    QJsonArray envelopes;
    QJsonArray eligible;
    const bool ready = credentials_.batch(
        [&](CredentialStore& view, QString* failure) {
            CloudCrypto crypto(&view);
            eligible = requirePins ? DevicePins(&view).trusted(devices, failure) : devices;
            if (failure && !failure->isEmpty())
                return false;
            for (const auto& value : eligible) {
                const auto device = value.toObject();
                for (const auto& session : sessions) {
                    if (refusedSessions_.contains(session.id))
                        continue;
                    const auto account =
                        QStringLiteral("wrapped-%1-%2-%3-%4")
                            .arg(session.id)
                            .arg(session.generation)
                            .arg(device.value(QStringLiteral("id")).toString())
                            .arg(device.value(QStringLiteral("keyVersion")).toInt());
                    const auto cached = view.read(account, failure);
                    if (failure && !failure->isEmpty())
                        return false;
                    auto envelope = QJsonDocument::fromJson(cached).object();
                    if (!CloudCrypto::validWrappedSessionKey(envelope)) {
                        envelope = crypto.wrapSessionKey(session.id, session.generation, 1, device,
                                                         machineId_, machineKeyVersion_, failure);
                        if (envelope.isEmpty() ||
                            !view.write(account,
                                        QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                                        failure))
                            return false;
                    }
                    envelopes.append(envelope);
                    if (limit > 0 && envelopes.size() >= limit)
                        return true;
                }
            }
            return true;
        },
        error);
    // Neither keys nor device refreshes are published until the whole batch commits.
    if (!ready)
        return {};
    if (trusted)
        *trusted = eligible;
    return envelopes;
}
} // namespace ketplus::daemon
