#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>

class QLabel;

namespace ket {
class TextField;
} // namespace ket

namespace ketplus {

// Settings → Teams (ADR 0114, open point 2): the automatic turns a new team starts with. The
// daemon keeps the value, so a team created from a phone or by an agent gets it too; changing
// it changes no existing team. It applies when the field is left, like the page's other fields.
class SettingsTeamsPage final : public QWidget {
    Q_OBJECT

  public:
    using Reply = std::function<void(const QJsonObject& result, const QString& error)>;
    // Sends team.settings.get or team.settings.update to the daemon.
    using Requester = std::function<void(const QString& type, const QJsonObject&, Reply)>;
    explicit SettingsTeamsPage(Requester requester = {}, QWidget* parent = nullptr);

    [[nodiscard]] ket::TextField* budgetField() const { return field_; }
    [[nodiscard]] QString error() const;
    void refresh();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void save();
    void receive(const QJsonObject& result, const QString& error);
    void showError(const QString& message);
    void paintError();

    Requester requester_;
    ket::TextField* field_;
    QLabel* error_;
    int saved_;
    bool pending_{false};
};

} // namespace ketplus
