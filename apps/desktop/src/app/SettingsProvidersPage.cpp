#include "app/SettingsProvidersPage.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "agent/ProviderSetupGuide.h"
#include "app/ProviderSetupCard.h"
#include "app/SettingsClaudeAnalytics.h"
#include "app/SettingsRows.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QHBoxLayout>
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

ket::BadgeTone statusTone(const QString& status) {
    if (status == QStringLiteral("supported") || status == QStringLiteral("bundled"))
        return ket::BadgeTone::Success;
    if (status == QStringLiteral("outdated") || status == QStringLiteral("missing"))
        return ket::BadgeTone::Danger;
    return ket::BadgeTone::Warning;
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
    layout->addSpacing(20);
    // The mock's status row: the summary, then Check again beside it.
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(12);
    status_ = textLabel(tr("Checking provider versions…"), QStringLiteral("providersStatus"), this);
    statusRow->addWidget(status_);
    auto* recheck = new ket::Button(tr("Check again"), ket::ButtonVariant::Secondary, this);
    recheck->setControlSize(ket::ControlSize::Small);
    recheck->setObjectName(QStringLiteral("providersRecheck"));
    statusRow->addWidget(recheck);
    statusRow->addStretch(1);
    layout->addLayout(statusRow);
    layout->addSpacing(20);
    list_ = new QWidget(this);
    listLayout_ = new QVBoxLayout(list_);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(20);
    layout->addWidget(list_);
    layout->addSpacing(20);
    layout->addWidget(settings_rows::divider(this));
    layout->addSpacing(20);
    layout->addWidget(new SettingsClaudeCredentials(credentials, this));
    layout->addWidget(new SettingsClaudeAnalytics(std::move(credentials), this));
    layout->addStretch(1);

    connect(recheck, &QAbstractButton::clicked, this, [this] { refresh(true); });
    QTimer::singleShot(0, this, [this] { refresh(); });
}

QString SettingsProvidersPage::statusText(const QString& status) {
    if (status == QStringLiteral("supported") || status == QStringLiteral("bundled"))
        return tr("Ready");
    if (status == QStringLiteral("newer"))
        return tr("Newer than tested");
    if (status == QStringLiteral("outdated"))
        return tr("Update required");
    if (status == QStringLiteral("missing"))
        return tr("Not installed");
    return tr("Version unknown");
}

void SettingsProvidersPage::refresh(const bool recheck) {
    if (!fetcher_) {
        apply({}, tr("The KetPlus daemon is not available."));
        return;
    }
    const quint64 generation = ++generation_;
    status_->setText(tr("Checking provider versions…"));
    const QPointer<SettingsProvidersPage> guard(this);
    fetcher_(recheck, [this, guard, generation](const QJsonArray& providers, const QString& error) {
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
        checking_.clear(); // a later refresh is not this check's answer
        status_->setText(tr("Provider versions could not be checked: %1").arg(error));
        emit refreshed();
        return;
    }
    int attention = 0;
    const QSet<QString> checked = std::exchange(checking_, {});
    for (const QJsonValue& value : providers) {
        const QJsonObject provider = value.toObject();
        const QString id = provider.value(QStringLiteral("provider")).toString();
        const QString status = provider.value(QStringLiteral("status")).toString();
        // A provider that cannot start a turn shows its setup guide (#170); a capacity or plan
        // error is about one model and never reaches Settings.
        QVariantMap guide = provider_setup::guide(provider);
        if (guide.value(QStringLiteral("transient")).toBool())
            guide.clear();
        if (!guide.isEmpty() || status == QStringLiteral("outdated") ||
            status == QStringLiteral("newer") || status == QStringLiteral("unknown"))
            ++attention;

        auto* card = new QWidget(list_);
        card->setObjectName(QStringLiteral("providerRuntime-%1")
                                .arg(provider.value(QStringLiteral("provider")).toString()));
        auto* grid = new QGridLayout(card);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(8);
        grid->setColumnMinimumWidth(0, 180); // the mock's InfoRow label column
        grid->setColumnStretch(1, 1);
        // The name with its status badge, as the mock's provider heading.
        auto* heading = new QHBoxLayout;
        heading->setSpacing(10);
        heading->addWidget(textLabel(provider.value(QStringLiteral("name")).toString(),
                                     QStringLiteral("settingsWorkspaceSubheading"), card));
        const bool guided = !guide.isEmpty();
        auto* badge = new ket::Badge(
            guided ? guide.value(QStringLiteral("badge")).toString() : statusText(status),
            !guided ? statusTone(status)
            : guide.value(QStringLiteral("tone")).toString() == QStringLiteral("danger")
                ? ket::BadgeTone::Danger
                : ket::BadgeTone::Warning,
            card);
        badge->setObjectName(QStringLiteral("providerRuntimeStatus"));
        badge->setProperty("providerStatus", status);
        badge->setProperty("setupId", guide.value(QStringLiteral("id")));
        heading->addWidget(badge);
        heading->addStretch(1);
        grid->addLayout(heading, 0, 0, 1, 2);
        int row = 1;
        const auto addRow = [card, grid, &row](const QString& label, QLabel* field) {
            grid->addWidget(textLabel(label, QStringLiteral("settingsWorkspaceFieldLabel"), card),
                            row, 0, Qt::AlignTop);
            grid->addWidget(field, row, 1);
            ++row;
        };
        if (guided) {
            // The guide's rows, then the guide itself, as the mock's provider section.
            for (const QVariant& entry : guide.value(QStringLiteral("rows")).toList()) {
                const QVariantList pair = entry.toList();
                addRow(pair.value(0).toString(),
                       selectableLabel(pair.value(1).toString(),
                                       QStringLiteral("providerRuntimeValue"), card));
            }
            auto* setup = new ProviderSetupCard(guide, card);
            if (checked.contains(id))
                setup->showStillFailing();
            connect(setup, &ProviderSetupCard::checkAgainRequested, this, [this, setup, id] {
                checking_.insert(id);
                setup->setChecking(true);
                refresh(true);
            });
            connect(setup, &ProviderSetupCard::typeInTerminalRequested, this,
                    &SettingsProvidersPage::typeInTerminalRequested);
            connect(setup, &ProviderSetupCard::actionRequested, this,
                    [this](const QString& action) {
                        emit sectionRequested(action.mid(QStringLiteral("settings/").size()));
                    });
            grid->addWidget(setup, row, 0, 1, 2);
            listLayout_->addWidget(card);
            continue;
        }
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
    int missing = 0;
    for (const QJsonValue& value : providers)
        missing += value.toObject().value(QStringLiteral("status")).toString() ==
                   QStringLiteral("missing");
    status_->setText(attention > 0  ? tr("%n provider(s) need attention.", nullptr, attention)
                     : missing == 0 ? tr("All providers are ready.")
                                    : tr("All installed providers are ready."));
    emit refreshed();
}

} // namespace ketplus
