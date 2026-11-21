#pragma once

#include "workspace/AtlasIndex.h"

#include <QProcessEnvironment>
#include <QStringList>

class QWidget;
namespace ket {
class Dialog;
}

namespace ketplus {

// A launch-local description, never a persisted grant. The CLI reads the contract again.
struct AtlasRendererPermission {
    QString manifestPath;
    QString contractPath;
    QString workingDirectory;
    QString executable;
    QStringList command;
    QByteArray manifest;
    QByteArray contract;
    QString error;
    bool present{false};

    static AtlasRendererPermission read(const AtlasDescriptor& atlas,
                                        const QProcessEnvironment& environment, quint16 port);
    bool operator==(const AtlasRendererPermission&) const = default;
};

ket::Dialog* rendererPermissionDialog(QWidget* anchor, const AtlasRendererPermission& permission);

} // namespace ketplus
