#include "teams/TeamAgentDialog.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSelect.h"

#include <QCoreApplication>
#include <QLabel>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamAgentDialog", text); }

QVariantList modelsOf(const QVariantList& models, const QString& provider) {
    QVariantList result;
    for (const QVariant& value : models)
        if (value.toMap().value(QStringLiteral("provider")).toString() == provider &&
            !value.toMap().value(QStringLiteral("hidden")).toBool() &&
            (provider != "copilot" || !value.toMap().value("permissions").toList().isEmpty()))
            result.append(value);
    return result;
}

QVariantMap entry(const QVariantList& models, const TeamAgent& agent) {
    for (const QVariant& value : modelsOf(models, agent.provider))
        if (value.toMap().value(QStringLiteral("model")).toString() == agent.model)
            return value.toMap();
    return {};
}

} // namespace

QJsonObject TeamAgent::json() const {
    QJsonObject result{{QStringLiteral("provider"), provider}};
    if (!model.isEmpty())
        result.insert(QStringLiteral("model"), model);
    if (!effort.isEmpty())
        result.insert(QStringLiteral("effort"), effort);
    return result;
}

QStringList team_agents::readyProviders(const QVariantList& models) {
    QStringList providers;
    for (const QVariant& value : models) {
        const QString provider = value.toMap().value(QStringLiteral("provider")).toString();
        if (value.toMap().value("hidden").toBool() ||
            (provider == "copilot" && value.toMap().value("permissions").toList().isEmpty()))
            continue;
        if (!provider.isEmpty() && !providers.contains(provider))
            providers.append(provider);
    }
    return providers;
}

QString team_agents::providerLabel(const QString& provider) {
    if (provider == QStringLiteral("claude"))
        return QStringLiteral("Claude");
    if (provider == QStringLiteral("codex"))
        return QStringLiteral("Codex");
    if (provider == QStringLiteral("cursor"))
        return QStringLiteral("Cursor");
    if (provider == QStringLiteral("copilot"))
        return QStringLiteral("GitHub Copilot");
    return provider;
}

TeamAgent team_agents::defaultFor(const QVariantList& models, const QString& provider) {
    const QVariantList own = modelsOf(models, provider);
    if (own.isEmpty())
        return {};
    QVariantMap chosen = own.first().toMap();
    for (const QVariant& value : own)
        if (value.toMap().value(QStringLiteral("isDefault")).toBool())
            chosen = value.toMap();
    return {provider, chosen.value(QStringLiteral("model")).toString(),
            chosen.value(QStringLiteral("defaultReasoningEffort")).toString()};
}

QString team_agents::label(const QVariantList& models, const TeamAgent& agent) {
    if (agent.isEmpty())
        return tr("Choose an agent");
    const QVariantMap model = entry(models, agent);
    QStringList parts{providerLabel(agent.provider)};
    const QString name = model.value(QStringLiteral("displayName")).toString();
    if (!name.isEmpty() || !agent.model.isEmpty())
        parts.append(name.isEmpty() ? agent.model : name);
    if (!agent.effort.isEmpty())
        parts.append(agent.effort);
    return parts.join(QStringLiteral(" · "));
}

team_agents::Defaults team_agents::defaults(const QVariantList& models) {
    const QStringList ready = readyProviders(models);
    Defaults result;
    result.singleProvider = ready.size() <= 1;
    const QString lead = ready.contains(QStringLiteral("claude"))
                             ? QStringLiteral("claude")
                             : (ready.isEmpty() ? QString{} : ready.first());
    QString develop = lead;
    for (const QString& provider :
         {QStringLiteral("codex"), QStringLiteral("cursor"), QStringLiteral("copilot")})
        if (provider != lead && ready.contains(provider)) {
            develop = provider;
            break;
        }
    result.leader = defaultFor(models, lead);
    result.developer = defaultFor(models, develop);
    result.reviewer = defaultFor(models, QLatin1String(reviewerProvider));
    return result;
}

TeamAgentChooser::TeamAgentChooser(const QVariantList& models, const TeamAgent& current,
                                   const QString& lockedProvider, QWidget* parent)
    : QWidget(parent), entries_(models), providers_(new ket::Select(this)),
      models_(new ket::Select(this)), efforts_(new ket::Select(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    providers_->setObjectName(QStringLiteral("teamAgentProvider"));
    models_->setObjectName(QStringLiteral("teamAgentModel"));
    efforts_->setObjectName(QStringLiteral("teamAgentEffort"));
    for (const QString& provider : team_agents::readyProviders(models))
        if (lockedProvider.isEmpty() || provider == lockedProvider)
            providers_->addItem(team_agents::providerLabel(provider), provider);
    QString help;
    if (!lockedProvider.isEmpty()) {
        providers_->setEnabled(false);
        help = tr("The reviewer runs on Claude, the agent that keeps a review read-only.");
        if (providers_->count() == 0)
            help = tr("The reviewer needs Claude. Set up Claude in Settings → Agents.");
    }
    layout->addWidget(new ket::FieldRow(tr("Agent"), providers_, help, this));
    layout->addWidget(new ket::FieldRow(tr("Model"), models_, {}, this));
    layout->addWidget(new ket::FieldRow(tr("Effort"), efforts_, {}, this));
    const int index = providers_->findData(current.provider);
    providers_->setCurrentIndex(index >= 0 ? index : 0);
    fillModels(current.model);
    fillEfforts(current.effort);
    connect(providers_, &QComboBox::currentIndexChanged, this, [this] {
        fillModels(team_agents::defaultFor(entries_, providers_->currentData().toString()).model);
        fillEfforts({});
    });
    connect(models_, &QComboBox::currentIndexChanged, this, [this] { fillEfforts({}); });
}

void TeamAgentChooser::fillModels(const QString& model) {
    const QSignalBlocker block(models_);
    models_->clear();
    for (const QVariant& value : modelsOf(entries_, providers_->currentData().toString())) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value(QStringLiteral("model")).toString();
        const QString name = entry.value(QStringLiteral("displayName")).toString();
        models_->addItem(name.isEmpty() ? id : name, id);
    }
    const int index = models_->findData(model);
    models_->setCurrentIndex(index >= 0 ? index : 0);
}

void TeamAgentChooser::fillEfforts(const QString& effort) {
    efforts_->clear();
    const QVariantMap model = entry(
        entries_, {providers_->currentData().toString(), models_->currentData().toString(), {}});
    for (const QVariant& value :
         model.value(QStringLiteral("supportedReasoningEfforts")).toList()) {
        const QString id = value.toMap().value(QStringLiteral("reasoningEffort")).toString();
        if (!id.isEmpty())
            efforts_->addItem(id, id);
    }
    const QString wanted = effort.isEmpty()
                               ? model.value(QStringLiteral("defaultReasoningEffort")).toString()
                               : effort;
    const int index = efforts_->findData(wanted);
    efforts_->setCurrentIndex(index >= 0 ? index : 0);
    efforts_->setEnabled(efforts_->count() > 1);
}

TeamAgent TeamAgentChooser::agent() const {
    return {providers_->currentData().toString(), models_->currentData().toString(),
            efforts_->currentData().toString()};
}

ket::Dialog* TeamAgentChooser::present(QWidget* anchor, const QString& role,
                                       const QVariantList& models, const TeamAgent& current,
                                       const QString& lockedProvider,
                                       std::function<void(const TeamAgent&)> chosen) {
    auto* dialog = new ket::Dialog(tr("Model & context · %1").arg(role), anchor);
    dialog->setObjectName(QStringLiteral("teamAgentDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* chooser = new TeamAgentChooser(models, current, lockedProvider);
    dialog->setBody(chooser);
    dialog->setCardWidth(420);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* use = dialog->addButton(tr("Use"), ket::ButtonVariant::Primary);
    use->setObjectName(QStringLiteral("teamAgentUse"));
    use->setEnabled(chooser->providers()->count() > 0);
    QObject::connect(use, &QAbstractButton::clicked, dialog,
                     [dialog, chooser, chosen = std::move(chosen)] {
                         chosen(chooser->agent());
                         dialog->accept();
                     });
    dialog->open();
    return dialog;
}

} // namespace ketplus
