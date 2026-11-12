#include "MachineBackend.h"

namespace ketplus::remote {

QString MachineBackend::unsupportedReason(const Feature feature) const {
    if (connectionState() != ConnectionState::Connected)
        return tr("Available once this machine is connected.");
    Q_UNUSED(feature);
    return tr("Not available on Linux machines yet.");
}

void MachineBackend::renameConversation(const QString&, qint64, const QString&, Callback callback) {
    callback({}, unsupportedReason(Feature::Rename));
}

void MachineBackend::forkConversation(const QString&, qint64, const QString&, Callback callback) {
    callback({}, unsupportedReason(Feature::Fork));
}

void MachineBackend::setConversationArchived(const QString&, qint64, bool, Callback callback) {
    callback({}, unsupportedReason(Feature::Archive));
}

} // namespace ketplus::remote
