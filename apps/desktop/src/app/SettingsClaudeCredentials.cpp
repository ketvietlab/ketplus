#include "app/SettingsClaudeCredentials.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

namespace ketplus {
SettingsClaudeCredentials::SettingsClaudeCredentials(Requester requester, QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)) {
    setObjectName(QStringLiteral("claudeCredentials"));
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(20);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 145);
    grid->setColumnStretch(1, 1);
    const auto label = [this](const QString& text, const QString& name) {
        auto* value = new QLabel(text, this);
        value->setObjectName(name);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        return value;
    };
    grid->addWidget(label(tr("Claude API access"), "settingsWorkspaceSubheading"), 0, 0, 1, 2);
    grid->addWidget(
        label(tr("Use your own Anthropic API key on this Mac. Paired phones use the same "
                 "configuration. Changes apply to new queries after current work finishes."),
              "settingsWorkspaceHelp"),
        1, 0, 1, 2);
    mode_ = new QComboBox(this);
    mode_->setObjectName("claudeAuthenticationMode");
    mode_->addItem(tr("Existing Claude runtime authentication"));
    mode_->addItem(tr("My Anthropic API key"));
    key_ = new QLineEdit(this);
    key_->setObjectName("claudeApiKey");
    key_->setEchoMode(QLineEdit::Password);
    key_->setMaxLength(512);
    key_->setPlaceholderText(tr("Enter a new key; leave blank to keep the saved key"));
    key_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    budget_ = new QDoubleSpinBox(this);
    budget_->setObjectName("claudeQueryBudget");
    budget_->setRange(0, 10000);
    budget_->setDecimals(2);
    budget_->setSuffix(tr(" USD"));
    budget_->setSpecialValueText(tr("No query limit"));
    const auto field = [grid, label](int row, const QString& text, QWidget* widget) {
        auto* name = label(text, "settingsWorkspaceFieldLabel");
        name->setBuddy(widget);
        grid->addWidget(name, row, 0);
        grid->addWidget(widget, row, 1);
    };
    field(2, tr("Authentication"), mode_);
    field(3, tr("API key"), key_);
    field(4, tr("Query budget"), budget_);
    grid->addWidget(label(tr("Stored in macOS Keychain. Usage is billed to the key's Anthropic "
                             "account. The query budget uses an estimate and can be exceeded; "
                             "check Anthropic Console for actual charges."),
                          "settingsWorkspaceHelp"),
                    5, 0, 1, 2);
    auto* actions = new QWidget(this);
    auto* buttons = new QGridLayout(actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    save_ = new QPushButton(tr("Save Claude settings"), actions);
    save_->setObjectName("claudeCredentialsSave");
    check_ = new QPushButton(tr("Check connection"), actions);
    check_->setObjectName("claudeCredentialsCheck");
    remove_ = new QPushButton(tr("Remove key…"), actions);
    remove_->setObjectName("claudeCredentialsRemove");
    refresh_ = new QPushButton(tr("Refresh"), actions);
    refresh_->setObjectName("claudeCredentialsRefresh");
    buttons->addWidget(save_, 0, 0);
    buttons->addWidget(check_, 0, 1);
    buttons->addWidget(remove_, 1, 0);
    buttons->addWidget(refresh_, 1, 1);
    grid->addWidget(actions, 6, 1, Qt::AlignLeft);
    status_ = label(tr("Loading Claude settings…"), "claudeCredentialsStatus");
    status_->setAccessibleName(tr("Claude credential status"));
    grid->addWidget(status_, 7, 0, 1, 2);
    observation_ = label({}, "claudeCredentialsObservation");
    grid->addWidget(observation_, 8, 0, 1, 2);
    connect(mode_, &QComboBox::currentIndexChanged, this,
            &SettingsClaudeCredentials::updateControls);
    connect(save_, &QPushButton::clicked, this, [this] {
        const QJsonObject payload{{"enabled", mode_->currentIndex() == 1},
                                  {"key", key_->text()},
                                  {"budget_usd", budget_->value()}};
        key_->clear();
        request("save", payload);
    });
    connect(check_, &QPushButton::clicked, this, [this] { request("check"); });
    connect(refresh_, &QPushButton::clicked, this, &SettingsClaudeCredentials::refresh);
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(
                this, tr("Remove Claude API key?"),
                tr("New queries will use the existing Claude runtime authentication. Running "
                   "queries finish with their current credentials."),
                QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel) == QMessageBox::Yes) {
            key_->clear();
            request("remove");
        }
    });
    updateControls();
    QTimer::singleShot(0, this, &SettingsClaudeCredentials::refresh);
}

void SettingsClaudeCredentials::refresh() {
    if (!busy_)
        request("status");
}

void SettingsClaudeCredentials::updateControls() {
    mode_->setEnabled(supported_ && !busy_);
    key_->setEnabled(supported_ && !busy_);
    budget_->setEnabled(supported_ && !busy_ && mode_->currentIndex() == 1);
    save_->setEnabled(supported_ && !busy_);
    check_->setEnabled(supported_ && !busy_ && configured_);
    remove_->setEnabled(supported_ && !busy_ && configured_);
    refresh_->setEnabled(!busy_);
}

void SettingsClaudeCredentials::request(const QString& operation, const QJsonObject& payload) {
    if (!requester_) {
        status_->setText(
            tr("Connect to an updated local KetPlus daemon to configure Claude API keys."));
        return;
    }
    busy_ = true;
    updateControls();
    status_->setText(operation == "check" ? tr("Checking Anthropic access…")
                                          : tr("Updating Claude settings…"));
    const QPointer<SettingsClaudeCredentials> guard(this);
    requester_(operation, payload,
               [this, guard, operation](const QJsonObject& result, const QString& error) {
                   if (!guard)
                       return;
                   busy_ = false;
                   if (!error.isEmpty()) {
                       status_->setText(error);
                   } else if (operation == "check") {
                       status_->setText(
                           tr("Anthropic accepted the saved key. No model inference was run."));
                   } else {
                       apply(result);
                   }
                   updateControls();
                   emit refreshed();
               });
}

void SettingsClaudeCredentials::apply(const QJsonObject& status) {
    supported_ = status.value("supported").toBool();
    configured_ = status.value("configured").toBool();
    mode_->setCurrentIndex(status.value("enabled").toBool() ? 1 : 0);
    budget_->setValue(status.value("budget_usd").toDouble());
    status_->setText(
        !supported_ ? tr("Saving API keys requires macOS Keychain in this release.")
        : status.value("enabled").toBool() ? tr("Saved API key selected for new queries. Running "
                                                "queries keep their current credentials.")
        : configured_ ? tr("Key saved. New queries use existing Claude runtime authentication.")
                      : tr("No API key saved. Using existing Claude runtime authentication."));
    const auto observed = status.value("observation").toObject();
    QString text;
    const auto source = observed.value("source").toString();
    if (!source.isEmpty())
        text =
            tr("Last observed credential: %1.")
                .arg(source == "none" ? tr("runtime login or other provider credential") : source);
    if (observed.contains("estimated_cost_usd"))
        text += tr(" Latest SDK session estimate: %1 USD.")
                    .arg(observed.value("estimated_cost_usd").toDouble(), 0, 'f', 4);
    observation_->setText(text);
}
} // namespace ketplus
