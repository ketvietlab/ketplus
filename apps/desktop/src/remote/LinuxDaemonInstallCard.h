#pragma once
#include <QString>
#include <memory>
class QWidget;
namespace ketplus {
class LinuxMachineModel;
QWidget* makeLinuxDaemonInstallCard(LinuxMachineModel* model, std::shared_ptr<QString> pendingHost,
                                    QWidget* parent);
} // namespace ketplus
