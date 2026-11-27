#include "teams/TeamRepositorySettings.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetTextField.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamRepositorySettings", text); }

ket::TextField* field(const QString& name, const QString& accessibleName, const QString& text,
                      QWidget* parent) {
    auto* edit = new ket::TextField(parent);
    edit->setObjectName(name);
    edit->setAccessibleName(accessibleName);
    edit->setText(text);
    return edit;
}

} // namespace

ket::Dialog* teamRepositorySettings(QWidget* anchor, TeamDirectory* teams,
                                    const QJsonObject& team) {
    const QString teamId = team.value(QStringLiteral("id")).toString();
    const QString repository = team.value(QStringLiteral("repository")).toString();
    const QString branch = team.value(QStringLiteral("integration_branch")).toString();
    auto* dialog = new ket::Dialog(
        tr("Repository for %1").arg(team.value(QStringLiteral("name")).toString()), anchor);
    dialog->setObjectName(QStringLiteral("teamRepositoryDialog"));
    // The kit's dialog hides when it finishes; this one is made for one change.
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(10));

    auto* pathLine = new QWidget(body);
    auto* pathLayout = new QHBoxLayout(pathLine);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->setSpacing(team_page::px(8));
    auto* path =
        field(QStringLiteral("teamRepositoryPath"), tr("Repository folder"), repository, pathLine);
    path->setPlaceholderText(QStringLiteral("/Users/you/dev/project"));
    auto* choose = new ket::Button(tr("Choose…"), ket::ButtonVariant::Ghost, pathLine);
    choose->setObjectName(QStringLiteral("teamRepositoryChoose"));
    pathLayout->addWidget(path, 1);
    pathLayout->addWidget(choose);
    QObject::connect(choose, &QAbstractButton::clicked, dialog, [dialog, path] {
        const QString chosen =
            QFileDialog::getExistingDirectory(dialog, tr("Choose the repository"), path->text());
        if (!chosen.isEmpty())
            path->setText(chosen);
    });
    layout->addWidget(new ket::FieldRow(
        tr("Repository"), pathLine,
        tr("The main checkout of a Git repository. New tasks and tasks not started yet work "
           "there. Started tasks keep the repository, branch and worktree they began in; nothing "
           "on disk moves."),
        body));

    auto* integration =
        field(QStringLiteral("teamRepositoryBranch"), tr("Integration branch"), branch, body);
    integration->setPlaceholderText(QStringLiteral("integration"));
    layout->addWidget(new ket::FieldRow(tr("Integration branch"), integration,
                                        tr("Its origin must have this branch. Fetch it first if "
                                           "it is new."),
                                        body));

    auto* reason = field(QStringLiteral("teamRepositoryReason"), tr("Reason"), {}, body);
    reason->setPlaceholderText(tr("Optional, shown in Updates"));
    layout->addWidget(new ket::FieldRow(tr("Reason"), reason, {}, body));

    auto* refusal = new ket::Banner(ket::BannerTone::Danger, tr("Not saved"), {}, body);
    refusal->setObjectName(QStringLiteral("teamRepositoryRefusal"));
    refusal->hide();
    layout->addWidget(refusal);
    dialog->setBody(body);
    dialog->setCardWidth(560);

    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* save = dialog->addButton(tr("Save"), ket::ButtonVariant::Primary);
    save->setObjectName(QStringLiteral("teamRepositorySave"));
    const QString key = TeamCommands::key(teamId, QStringLiteral("repository"));
    teams->commands()->bind(save, key);
    QObject::connect(
        save, &QAbstractButton::clicked, dialog,
        [dialog, teams, teamId, key, path, integration, reason, refusal, repository, branch] {
            const QString chosen = path->text().trimmed();
            const QString chosenBranch = integration->text().trimmed();
            if (chosen == repository && chosenBranch == branch) {
                dialog->accept();
                return;
            }
            if (teams->commands()->isPending(key))
                return;
            QJsonObject payload{{QStringLiteral("team_id"), teamId},
                                {QStringLiteral("repository"), chosen},
                                {QStringLiteral("integration_branch"), chosenBranch}};
            if (!reason->text().trimmed().isEmpty())
                payload.insert(QStringLiteral("reason"), reason->text().trimmed());
            const QPointer<ket::Dialog> alive(dialog);
            teams->commands()->send(
                key, {}, QStringLiteral("team.update"), payload,
                [alive, refusal](const QJsonObject& result, const QString& failure) {
                    if (!alive)
                        return;
                    if (failure.isEmpty()) {
                        alive->accept();
                        return;
                    }
                    const QString message =
                        result.value(QStringLiteral("error_message")).toString();
                    refusal->setMessage(message.isEmpty() ? failure : message);
                    refusal->show();
                });
        });
    dialog->open();
    return dialog;
}

} // namespace ketplus
