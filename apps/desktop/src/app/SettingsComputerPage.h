#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace ketplus {

// Settings → Computer use (ADR 0026): the opt-in for ketplus_computer and the user's deny list.
// KetPlus Computer itself asks for per-app approval in every conversation; this page only decides
// whether agents receive the tool at all. Changes apply to the next turn.
class SettingsComputerPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsComputerPage(QString settingsPath = {}, QString applicationDir = {},
                                  QWidget* parent = nullptr);

  private:
    void load();
    void saveEnabled(bool enabled);
    void saveDeniedApps();

    QString settingsPath_;
    QString applicationDir_;
    QLabel* status_{nullptr};
    QCheckBox* enabled_{nullptr};
    QLineEdit* deniedApps_{nullptr};
    QLabel* error_{nullptr};
};

} // namespace ketplus
