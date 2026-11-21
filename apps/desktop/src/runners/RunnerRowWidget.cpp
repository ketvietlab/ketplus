#include "runners/RunnerRowWidget.h"

#include "KetBadge.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "runners/RunnerMessages.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace ketplus::runners {
namespace {

constexpr int shownLabels = 6;

QLabel* plainLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    return label;
}

ket::BadgeTone relationTone(const Relation relation) {
    switch (relation) {
    case Relation::Trusted:
        return ket::BadgeTone::Success;
    case Relation::NotPaired:
        return ket::BadgeTone::Neutral;
    case Relation::PinnedOnly:
    case Relation::PairedOnly:
    case Relation::Disputed:
        break;
    }
    return ket::BadgeTone::Warning;
}

ket::BadgeTone stateTone(const QString& state) {
    if (state == QStringLiteral("idle"))
        return ket::BadgeTone::Success;
    if (state == QStringLiteral("busy"))
        return ket::BadgeTone::Info;
    if (state == QStringLiteral("draining"))
        return ket::BadgeTone::Warning;
    if (state == QStringLiteral("removed"))
        return ket::BadgeTone::Danger;
    return ket::BadgeTone::Neutral;
}

QString platformLine(const RunnerRow& row, const QDateTime& now) {
    QStringList parts;
    const QString os = plain(row.os, 24);
    const QString arch = plain(row.arch, 24);
    if (!os.isEmpty() || !arch.isEmpty())
        parts << QStringList{os, arch}.join(QLatin1Char(' ')).trimmed();
    const QString version = plain(row.version, 32);
    if (!version.isEmpty())
        parts << RunnerRowWidget::tr("Version %1").arg(version);
    parts << lastSeenText(row.lastSeen, now);
    return parts.join(QStringLiteral(" · "));
}

} // namespace

RunnerRowWidget::RunnerRowWidget(const RunnerRow& row, const QDateTime& now, QWidget* parent)
    : QWidget(parent), id_(row.id), name_(plain(row.name)) {
    if (name_.isEmpty())
        name_ = row.id;
    setObjectName(QStringLiteral("runnerRow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 10);
    layout->setSpacing(6);

    auto* heading = new QHBoxLayout;
    heading->setSpacing(8);
    auto* name = plainLabel(name_, QStringLiteral("settingsWorkspaceFieldLabel"), this);
    name->setAccessibleName(tr("Runner %1").arg(name_));
    heading->addWidget(name, 1);
    auto* relation = new ket::Badge(relationText(row.relation), relationTone(row.relation), this);
    relation->setObjectName(QStringLiteral("runnerRelation"));
    relation->setAccessibleName(relationText(row.relation));
    heading->addWidget(relation);
    auto* state = new ket::Badge(stateText(row.state), stateTone(row.state), this);
    state->setObjectName(QStringLiteral("runnerState"));
    state->setAccessibleName(stateText(row.state));
    heading->addWidget(state);
    layout->addLayout(heading);

    layout->addWidget(
        plainLabel(platformLine(row, now), QStringLiteral("settingsWorkspaceHelp"), this));

    if (!row.labels.isEmpty()) {
        auto* labels = new QHBoxLayout;
        labels->setSpacing(4);
        for (int index = 0; index < row.labels.size() && index < shownLabels; ++index) {
            auto* badge =
                new ket::Badge(plain(row.labels.at(index), 32), ket::BadgeTone::Neutral, this);
            badge->setObjectName(QStringLiteral("runnerLabel"));
            labels->addWidget(badge);
        }
        if (row.labels.size() > shownLabels) {
            auto* more = new ket::Badge(tr("+%1").arg(row.labels.size() - shownLabels),
                                        ket::BadgeTone::Neutral, this);
            more->setObjectName(QStringLiteral("runnerLabel"));
            labels->addWidget(more);
        }
        labels->addStretch(1);
        layout->addLayout(labels);
    }

    const QString warning = relationWarning(row.relation);
    if (!warning.isEmpty()) {
        auto* banner =
            new ket::Banner(ket::BannerTone::Warning, relationText(row.relation), warning, this);
        banner->setObjectName(QStringLiteral("runnerMismatch"));
        layout->addWidget(banner);
    }

    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    const auto add = [this, actions](const RowAction action, const QString& text,
                                     const QString& accessible, const ket::ButtonVariant variant) {
        auto* button = new ket::Button(text, variant, this);
        button->setControlSize(ket::ControlSize::Small);
        button->setAccessibleName(accessible);
        button->setFocusPolicy(Qt::StrongFocus);
        actions->addWidget(button);
        addAction(action, button);
    };
    if (row.canPair())
        add(RowAction::Pair, tr("Pair"), tr("Pair %1 with this computer").arg(name_),
            ket::ButtonVariant::Secondary);
    if (row.canEnable())
        add(RowAction::Enable, tr("Enable"), tr("Enable %1").arg(name_),
            ket::ButtonVariant::Secondary);
    else if (row.canDisable())
        add(RowAction::Disable, tr("Disable"), tr("Disable %1").arg(name_),
            ket::ButtonVariant::Secondary);
    if (row.canUnpair())
        add(RowAction::Unpair, tr("Unpair"), tr("Unpair %1 from this computer").arg(name_),
            ket::ButtonVariant::Ghost);
    if (row.canRemove())
        add(RowAction::Remove, tr("Remove"), tr("Remove %1 from your account").arg(name_),
            ket::ButtonVariant::GhostDanger);
    actions->addStretch(1);
    layout->addLayout(actions);
}

void RunnerRowWidget::addAction(const RowAction action, ket::Button* button) {
    buttons_.append({action, button});
    connect(button, &ket::Button::clicked, this, [this, action] {
        if (!busy_)
            emit actionRequested(action, id_);
    });
}

void RunnerRowWidget::setBusy(const bool busy) {
    busy_ = busy;
    for (const auto& [action, button] : std::as_const(buttons_))
        button->setEnabled(!busy);
}

ket::Button* RunnerRowWidget::button(const RowAction action) const {
    for (const auto& [kind, button] : buttons_)
        if (kind == action)
            return button;
    return nullptr;
}

} // namespace ketplus::runners
