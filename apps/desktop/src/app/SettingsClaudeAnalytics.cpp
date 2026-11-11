#include "app/SettingsClaudeAnalytics.h"
#include "KetButton.h"
#include "KetTextField.h"
#include "app/SettingsRows.h"
#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
namespace ketplus {
SettingsClaudeAnalytics::SettingsClaudeAnalytics(SettingsClaudeCredentials::Requester requester,
                                                 QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 20, 0, 0);
    column->setSpacing(14);
    const auto label = [this](const QString& text, const QString& name) {
        auto* result = new QLabel(text, this);
        result->setObjectName(name);
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        return result;
    };
    // The mock's section: a divider, the heading and help, the key's row and one row of actions.
    column->addWidget(settings_rows::divider(this));
    column->addWidget(label(tr("Claude organization usage"), "settingsWorkspaceSubheading"));
    column->addWidget(label(tr("Optional Admin API key for organization-wide monthly usage and "
                               "cost. Stored separately in macOS Keychain; never sent to the "
                               "agent. This key may belong to a different organization from "
                               "your inference key. Reports include all organization products "
                               "and keys."),
                            "settingsWorkspaceHelp"));
    key_ = new ket::TextField(this);
    key_->setObjectName("claudeAdminKey");
    key_->setEchoMode(QLineEdit::Password);
    key_->setMaxLength(512);
    key_->setPlaceholderText(tr("sk-ant-admin01-…"));
    key_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    column->addWidget(settings_rows::field(tr("Admin API key"), {}, key_, this));
    auto* actions = new QWidget(this);
    auto* buttons = new QHBoxLayout(actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    save_ = new ket::Button(tr("Save Admin key"), ket::ButtonVariant::Secondary, actions);
    save_->setObjectName("claudeAdminSave");
    remove_ = new ket::Button(tr("Remove Admin key…"), ket::ButtonVariant::GhostDanger, actions);
    remove_->setObjectName("claudeAdminRemove");
    refresh_ =
        new ket::Button(tr("Refresh organization usage"), ket::ButtonVariant::Ghost, actions);
    refresh_->setObjectName("claudeAdminRefresh");
    for (QWidget* button : {save_, remove_, refresh_})
        buttons->addWidget(button);
    buttons->addStretch(1);
    column->addWidget(actions);
    status_ = label(tr("Loading organization settings…"), "claudeAdminStatus");
    column->addWidget(status_);
    column->addWidget(
        label(tr("Month-to-date is measured in UTC. Cost is after discounts and before "
                 "credits; it is not your credit balance. Refreshes are cached for five minutes. "
                 "Billing balance and account spending limits remain in Anthropic Console."),
              "settingsWorkspaceHelp"));
    connect(save_, &QAbstractButton::clicked, this, [this] {
        const QJsonObject payload{{"key", key_->text()}};
        key_->clear();
        request("save", payload);
    });
    connect(refresh_, &QAbstractButton::clicked, this, [this] { request("refresh"); });
    connect(remove_, &QAbstractButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove Admin API key?"),
                                  tr("Organization reports will be disconnected. Your inference "
                                     "API key is unchanged."),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) == QMessageBox::Yes) {
            key_->clear();
            request("remove");
        }
    });
    updateControls();
    QTimer::singleShot(0, this, [this] { request("status"); });
}
void SettingsClaudeAnalytics::updateControls() {
    key_->setEnabled(supported_ && !busy_);
    save_->setEnabled(supported_ && !busy_);
    remove_->setEnabled(configured_ && !busy_);
    refresh_->setEnabled(!busy_);
}
void SettingsClaudeAnalytics::request(const QString& operation, const QJsonObject& payload) {
    if (!requester_) {
        status_->setText(tr("Connect to the updated local daemon."));
        return;
    }
    busy_ = true;
    updateControls();
    status_->setText(tr("Loading organization usage…"));
    const QPointer<SettingsClaudeAnalytics> guard(this);
    requester_("analytics." + operation, payload,
               [this, guard](const QJsonObject& result, const QString& error) {
                   if (!guard)
                       return;
                   busy_ = false;
                   if (!error.isEmpty())
                       status_->setText(error);
                   else {
                       supported_ = result.value("supported").toBool();
                       configured_ = result.value("configured").toBool();
                       const auto report = result.value("report").toObject();
                       status_->setText(
                           !configured_ ? tr("No Admin API key saved.")
                           : report.isEmpty()
                               ? tr("Admin key saved. Refresh to check access and load "
                                    "organization usage.")
                               : tr("Organization month-to-date: %1 USD · %2 tokens. Updated %3.")
                                     .arg(report.value("costUsd").toDouble(), 0, 'f', 2)
                                     .arg(report.value("totalTokens").toInteger())
                                     .arg(report.value("fetchedAt").toString()));
                   }
                   updateControls();
               });
}
} // namespace ketplus
