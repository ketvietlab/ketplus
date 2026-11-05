#include "app/SettingsClaudeAnalytics.h"
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
namespace ketplus {
SettingsClaudeAnalytics::SettingsClaudeAnalytics(SettingsClaudeCredentials::Requester requester,
                                                 QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 16, 0, 0);
    grid->setHorizontalSpacing(20);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 145);
    grid->setColumnStretch(1, 1);
    const auto label = [this](const QString& text, const QString& name) {
        auto* result = new QLabel(text, this);
        result->setObjectName(name);
        result->setTextFormat(Qt::PlainText);
        result->setWordWrap(true);
        return result;
    };
    grid->addWidget(label(tr("Claude organization usage"), "settingsWorkspaceSubheading"), 0, 0, 1,
                    2);
    grid->addWidget(label(tr("Optional Admin API key for organization-wide monthly usage and cost. "
                             "Stored separately in macOS Keychain; never sent to the agent. This "
                             "key may belong to a "
                             "different organization from your inference key. Reports include all "
                             "organization products and keys."),
                          "settingsWorkspaceHelp"),
                    1, 0, 1, 2);
    key_ = new QLineEdit(this);
    key_->setObjectName("claudeAdminKey");
    key_->setEchoMode(QLineEdit::Password);
    key_->setMaxLength(512);
    key_->setPlaceholderText(tr("sk-ant-admin01-…"));
    key_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    auto* name = label(tr("Admin API key"), "settingsWorkspaceFieldLabel");
    name->setBuddy(key_);
    grid->addWidget(name, 2, 0);
    grid->addWidget(key_, 2, 1);
    auto* actions = new QWidget(this);
    auto* buttons = new QGridLayout(actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    save_ = new QPushButton(tr("Save Admin key"), this);
    save_->setObjectName("claudeAdminSave");
    remove_ = new QPushButton(tr("Remove Admin key…"), this);
    remove_->setObjectName("claudeAdminRemove");
    refresh_ = new QPushButton(tr("Refresh organization usage"), this);
    refresh_->setObjectName("claudeAdminRefresh");
    buttons->addWidget(save_, 0, 0);
    buttons->addWidget(remove_, 0, 1);
    buttons->addWidget(refresh_, 1, 0, 1, 2);
    grid->addWidget(actions, 3, 1, Qt::AlignLeft);
    status_ = label(tr("Loading organization settings…"), "claudeAdminStatus");
    grid->addWidget(status_, 4, 0, 1, 2);
    grid->addWidget(
        label(tr("Month-to-date is measured in UTC. Cost is after discounts and before "
                 "credits; it is not your credit balance. Refreshes are cached for five minutes. "
                 "Billing balance and account spending limits remain in Anthropic Console."),
              "settingsWorkspaceHelp"),
        5, 0, 1, 2);
    connect(save_, &QPushButton::clicked, this, [this] {
        const QJsonObject payload{{"key", key_->text()}};
        key_->clear();
        request("save", payload);
    });
    connect(refresh_, &QPushButton::clicked, this, [this] { request("refresh"); });
    connect(remove_, &QPushButton::clicked, this, [this] {
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
