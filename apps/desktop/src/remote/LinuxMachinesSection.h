#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QAbstractButton;
class QVBoxLayout;

namespace ketplus::remote {
class ControlV2Client;
}

namespace ketplus {

class LinuxMachinesSection final : public QWidget {
    Q_OBJECT
  public:
    explicit LinuxMachinesSection(QWidget* parent = nullptr);
    void activate();
    void beginPairing();
    [[nodiscard]] QStringList machineIds() const;

  signals:
    void machinesChanged(const QStringList& ids, const QStringList& names);

  private:
    void render();
    void signIn();
    void startPairing();
    void refreshMachines();
    void setNotice(const QString& message, bool error = false);

    remote::ControlV2Client* client_;
    QLabel* count_;
    QLabel* notice_;
    QVBoxLayout* rows_;
    QAbstractButton* add_;
    QStringList machineIds_;
    bool loading_{false};
};

} // namespace ketplus
