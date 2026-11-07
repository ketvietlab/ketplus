#include "app/SettingsProvidersPage.h"
#include "KetButton.h"
#include "app/SettingsClaudeAnalytics.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

QLabel* selectableLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = textLabel(text, objectName, parent);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return value;
}

QString versionText(const QJsonObject& provider) {
    const QJsonObject components = provider.value(QStringLiteral("components")).toObject();
    const QString claudeCode = components.value(QStringLiteral("claude_code")).toString();
    const QString version = provider.value(QStringLiteral("version")).toString();
    if (!claudeCode.isEmpty())
        return SettingsProvidersPage::tr("Agent SDK %1 · Claude Code %2")
            .arg(version.isEmpty() ? QStringLiteral("?") : version, claudeCode);
    return version.isEmpty() ? SettingsProvidersPage::tr("Unknown") : version;
}

} // namespace

SettingsProvidersPage::SettingsProvidersPage(Fetcher fetcher, QWidget* parent,
                                             SettingsClaudeCredentials::Requester credentials)
    : QWidget(parent), fetcher_(std::move(fetcher)) {
    setObjectName(QStringLiteral("settingsWorkspaceProviders"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Providers"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(7);
    layout->addWidget(textLabel(tr("Codex and Cursor run from the CLIs installed on this "
                                   "computer; Claude runs from the Agent SDK bundled with "
                                   "KetPlus. KetPlus supports CLI releases from about the last "
                                   "three months."),
                                QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(24);
    status_ = textLabel(tr("Checking provider versions…"), QStringLiteral("providersStatus"), this);
    layout->addWidget(status_);
    layout->addSpacing(8);
    auto* recheck = new ket::Button(tr("Check again"), ket::ButtonVariant::Secondary, this);
    recheck->setObjectName(QStringLiteral("providersRecheck"));
    layout->addWidget(recheck, 0, Qt::AlignLeft);
    layout->addSpacing(20);
    list_ = new QWidget(this);
    listLayout_ = new QVBoxLayout(list_);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(24);
    layout->addWidget(list_);
    layout->addSpacing(24);
    layout->addWidget(new SettingsClaudeCredentials(credentials, this));
    layout->addWidget(new SettingsClaudeAnalytics(std::move(credentials), this));
    layout->addStretch(1);

    connect(recheck, &QAbstractButton::clicked, this, &SettingsProvidersPage::refresh);
    QTimer::singleShot(0, this, &SettingsProvidersPage::refresh);
}

QString SettingsProvidersPage::statusText(const QString& status) {
    if (status == QStringLiteral("supported"))
        return tr("Supported");
    if (status == QStringLiteral("newer"))
        return tr("Newer than tested");
    if (status == QStringLiteral("outdated"))
        return tr("Update required");
    if (status == QStringLiteral("missing"))
        return tr("Not installed");
    if (status == QStringLiteral("bundled"))
        return tr("Bundled with KetPlus");
    return tr("Version unknown");
}

void SettingsProvidersPage::refresh() {
    if (!fetcher_) {
        apply({}, tr("The KetPlus daemon is not available."));
        return;
    }
    const quint64 generation = ++generation_;
    status_->setText(tr("Checking provider versions…"));
    const QPointer<SettingsProvidersPage> guard(this);
    fetcher_([this, guard, generation](const QJsonArray& providers, const QString& error) {
        if (guard.isNull() || generation != generation_)
            return;
        apply(providers, error);
    });
}

void SettingsProvidersPage::apply(const QJsonArray& providers, const QString& error) {
    providers_ = providers;
    while (QLayoutItem* item = listLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!error.isEmpty()) {
        status_->setText(tr("Provider versions could not be checked: %1").arg(error));
        emit refreshed();
        return;
    }
    int attention = 0;
    for (const QJsonValue& value : providers) {
        const QJsonObject provider = value.toObject();
        const QString status = provider.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("outdated") || status == QStringLiteral("newer") ||
            status == QStringLiteral("unknown"))
            ++attention;

        auto* card = new QWidget(list_);
        card->setObjectName(QStringLiteral("providerRuntime-%1")
                                .arg(provider.value(QStringLiteral("provider")).toString()));
        auto* grid = new QGridLayout(card);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(20);
        grid->setVerticalSpacing(8);
        grid->setColumnMinimumWidth(0, 145);
        grid->setColumnStretch(1, 1);
        grid->addWidget(textLabel(provider.value(QStringLiteral("name")).toString(),
                                  QStringLiteral("settingsWorkspaceSubheading"), card),
                        0, 0, 1, 2);
        int row = 1;
        const auto addRow = [card, grid, &row](const QString& label, QLabel* field) {
            grid->addWidget(textLabel(label, QStringLiteral("settingsWorkspaceFieldLabel"), card),
                            row, 0, Qt::AlignTop);
            grid->addWidget(field, row, 1);
            ++row;
        };
        auto* statusLabel =
            textLabel(statusText(status), QStringLiteral("providerRuntimeStatus"), card);
        statusLabel->setProperty("providerStatus", status);
        addRow(tr("Status"), statusLabel);
        if (status != QStringLiteral("missing"))
            addRow(tr("Version"), selectableLabel(versionText(provider),
                                                  QStringLiteral("providerRuntimeVersion"), card));
        const QString minimum = provider.value(QStringLiteral("minimum")).toString();
        const QString newest = provider.value(QStringLiteral("newest_tested")).toString();
        if (!minimum.isEmpty())
            addRow(tr("Supported"),
                   textLabel(tr("%1 or newer · tested up to %2").arg(minimum, newest),
                             QStringLiteral("providerRuntimeRange"), card));
        const QString executable = provider.value(QStringLiteral("executable")).toString();
        if (!executable.isEmpty())
            addRow(tr("Location"),
                   selectableLabel(executable, QStringLiteral("providerRuntimePath"), card));
        const QString update = provider.value(QStringLiteral("update_command")).toString();
        if (!update.isEmpty() && status != QStringLiteral("bundled"))
            addRow(tr("Update with"),
                   selectableLabel(update, QStringLiteral("providerRuntimeUpdate"), card));
        const QString message = provider.value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            grid->addWidget(textLabel(message, QStringLiteral("settingsWorkspaceHelp"), card), row,
                            0, 1, 2);
        listLayout_->addWidget(card);
    }
    status_->setText(attention == 0 ? tr("All installed providers are supported.")
                                    : tr("%n provider(s) need attention.", nullptr, attention));
    emit refreshed();
}

} // namespace ketplus
