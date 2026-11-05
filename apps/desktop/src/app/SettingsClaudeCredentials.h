#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
class QLabel;
class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;

namespace ketplus {
class SettingsClaudeCredentials final : public QWidget {
    Q_OBJECT
  public:
    using Reply = std::function<void(const QJsonObject&, const QString&)>;
    using Requester = std::function<void(const QString&, const QJsonObject&, Reply)>;
    explicit SettingsClaudeCredentials(Requester requester, QWidget* parent = nullptr);
    void refresh();
  signals:
    void refreshed();

  private:
    void request(const QString& operation, const QJsonObject& payload = {});
    void apply(const QJsonObject& status);
    void updateControls();
    Requester requester_;
    QComboBox* mode_;
    QLineEdit* key_;
    QDoubleSpinBox* budget_;
    QLabel* status_;
    QLabel* observation_;
    QPushButton* save_;
    QPushButton* check_;
    QPushButton* remove_;
    QPushButton* refresh_;
    bool busy_{false};
    bool supported_{false};
    bool configured_{false};
};
} // namespace ketplus
