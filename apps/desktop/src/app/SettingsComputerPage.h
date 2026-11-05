#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace ketplus {

// Settings → Computer use (ADR 0026, 0028): the opt-in for ketplus_computer, the work display and
// the user's deny list.
// KetPlus Computer itself asks for per-app approval in every conversation; this page decides
// whether agents receive the tool and where they work. The opt-in applies to the next turn; the
// agent reads the display and deny list on every call.
class SettingsComputerPage final : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsComputerPage(QString settingsPath = {}, QString applicationDir = {},
                                  QWidget* parent = nullptr);

  private:
    void load();
    void saveEnabled(bool enabled);
    void saveDeniedApps();
    void saveDisplay(int index);

    QString settingsPath_;
    QString applicationDir_;
    QLabel* status_{nullptr};
    QCheckBox* enabled_{nullptr};
    QLineEdit* deniedApps_{nullptr};
    QComboBox* display_{nullptr};
    QLabel* error_{nullptr};
};

} // namespace ketplus
