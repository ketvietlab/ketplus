#pragma once
#include "app/SettingsClaudeCredentials.h"
namespace ketplus {
class SettingsClaudeAnalytics final : public QWidget {
    Q_OBJECT
  public:
    explicit SettingsClaudeAnalytics(SettingsClaudeCredentials::Requester requester,
                                     QWidget* parent = nullptr);

  private:
    void request(const QString& operation, const QJsonObject& payload = {});
    void updateControls();
    SettingsClaudeCredentials::Requester requester_;
    QLineEdit* key_;
    QLabel* status_;
    QPushButton* save_;
    QPushButton* remove_;
    QPushButton* refresh_;
    bool busy_{false}, configured_{false}, supported_{false};
};
} // namespace ketplus
