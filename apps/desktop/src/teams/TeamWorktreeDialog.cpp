#include "teams/TeamWorktreeDialog.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"

#include <QCoreApplication>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

using team_page::Text;

QString tr(const char* text) { return QCoreApplication::translate("TeamWorktreeDialog", text); }

constexpr qint64 kGigabyte = 1000LL * 1000 * 1000;

} // namespace

QString team_storage::size(const qint64 bytes) {
    if (bytes >= kGigabyte)
        return tr("%1 GB").arg(static_cast<double>(bytes) / kGigabyte, 0, 'f', 1);
    return tr("%1 MB").arg(static_cast<double>(bytes) / (1000.0 * 1000.0), 0, 'f', 1);
}

team_storage::Warning team_storage::warning(const QJsonObject& storage) {
    const QString level = storage.value(QStringLiteral("level")).toString();
    const QJsonValue free = storage.value(QStringLiteral("free_bytes"));
    if (level.isEmpty() || !free.isDouble())
        return {};
    const QString freeText = size(free.toInteger());
    const auto gigabytes = [&](const char* key) {
        return QString::number(storage.value(QLatin1String(key)).toInteger() / kGigabyte);
    };
    if (level == QStringLiteral("stop"))
        return {.stop = true,
                .title = tr("Builds should stop: %1 free").arg(freeText),
                .message = tr("The disk that holds this team's worktrees has less than %1 GB "
                              "free, so a build may fill it. Free space before the next build; "
                              "nothing is paused or removed for you.")
                               .arg(gigabytes("stop_bytes"))};
    return {.stop = false,
            .title = tr("Disk space is low: %1 free").arg(freeText),
            .message = tr("The disk that holds this team's worktrees has less than %1 GB free. "
                          "Remove the worktrees of done tasks below, or free space, before "
                          "builds fill it.")
                           .arg(gigabytes("warn_bytes"))};
}

ket::Dialog* teamWorktreeRemoval(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                 const QJsonObject& entry) {
    const QString taskId = entry.value(QStringLiteral("task_id")).toString();
    auto* dialog = new ket::Dialog(tr("Remove the worktree of %1?").arg(taskId), anchor);
    dialog->setObjectName(QStringLiteral("teamWorktreeDialog"));
    // The kit's dialog hides when it finishes; this one is made for one removal.
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(10));
    const QJsonValue bytes = entry.value(QStringLiteral("size_bytes"));
    auto* consequence = new Text(
        bytes.isDouble()
            ? tr("Its folder is deleted from disk, freeing %1. The task's branch and its commits "
                 "stay.")
                  .arg(team_storage::size(bytes.toInteger()))
            : tr("Its folder is deleted from disk. The task's branch and its commits stay."),
        body);
    consequence->setObjectName(QStringLiteral("teamWorktreeConsequence"));
    consequence->setTone(QStringLiteral("text-secondary"));
    layout->addWidget(consequence);
    auto* path = new Text(entry.value(QStringLiteral("path")).toString(), body);
    path->setObjectName(QStringLiteral("teamWorktreePath"));
    path->setType(12, QFont::Normal, true);
    layout->addWidget(path);
    auto* how = new Text(tr("ketPlus runs git worktree remove, without force, and then git "
                            "worktree prune from the repository's main checkout. It refuses a "
                            "worktree with uncommitted or untracked changes or with commits on "
                            "no remote branch, the main checkout, and the folder of a "
                            "conversation that is running or waits for you. An idle "
                            "conversation in the folder moves to the main checkout."),
                         body);
    how->setTone(QStringLiteral("text-muted"));
    how->setType(12);
    layout->addWidget(how);
    auto* refusal = new ket::Banner(ket::BannerTone::Danger, tr("Not removed"), {}, body);
    refusal->setObjectName(QStringLiteral("teamWorktreeRefusal"));
    refusal->hide();
    layout->addWidget(refusal);
    // After a removal: a conversation that could not be moved out of the folder, in words.
    auto* note = new ket::Banner(ket::BannerTone::Warning, tr("Removed, but a conversation stayed"),
                                 {}, body);
    note->setObjectName(QStringLiteral("teamWorktreeNote"));
    note->hide();
    layout->addWidget(note);
    dialog->setBody(body);
    dialog->setCardWidth(500);

    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    cancel->setObjectName(QStringLiteral("teamWorktreeCancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* remove = dialog->addButton(tr("Remove worktree"), ket::ButtonVariant::Danger);
    remove->setObjectName(QStringLiteral("teamWorktreeRemove"));
    QObject::connect(remove, &QAbstractButton::clicked, dialog, [=] {
        // Sent once: a refusal changes nothing, so the same removal would be refused again.
        remove->setEnabled(false);
        const QPointer<ket::Dialog> alive(dialog);
        teams->request(QStringLiteral("team.worktree.remove"),
                       {{QStringLiteral("team_id"), teamId}, {QStringLiteral("task_id"), taskId}},
                       [alive, refusal, note, remove, cancel](const QJsonObject& result,
                                                              const QString& error) {
                           if (!alive)
                               return;
                           const QString said = result.value(QStringLiteral("note")).toString();
                           if (error.isEmpty() && said.isEmpty()) {
                               alive->accept();
                               return;
                           }
                           if (error.isEmpty()) {
                               // Removed: what stays to do is the user's, so the dialog says it.
                               note->setMessage(said);
                               note->show();
                               remove->hide();
                               cancel->setText(tr("Close"));
                               return;
                           }
                           refusal->setMessage(error);
                           refusal->show();
                       });
    });
    return dialog;
}

} // namespace ketplus
