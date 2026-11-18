#include "runners/SettingsRunnersPage.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetEmptyState.h"
#include "app/SettingsRows.h"
#include "runners/RunnerDialogText.h"
#include "runners/RunnerMessages.h"
#include "runners/RunnerPairingDialog.h"

#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

using runners::RowAction;
using runners::Unavailable;

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

QString iconFor(const Unavailable unavailable) {
    switch (unavailable) {
    case Unavailable::NotEnrolled:
        return QStringLiteral("lock");
    case Unavailable::Network:
        return QStringLiteral("wifi-off");
    case Unavailable::DaemonUnsupported:
        return QStringLiteral("download");
    case Unavailable::RelayUnavailable:
    case Unavailable::NoEntitlement:
    case Unavailable::DaemonUnreachable:
    case Unavailable::None:
        break;
    }
    return QStringLiteral("warning");
}

QString failureCode(const QJsonObject& result) {
    return result.value(QStringLiteral("error_code")).toString();
}

QString failureMessage(const QJsonObject& result, const QString& error) {
    const QString message = result.value(QStringLiteral("error_message")).toString();
    return message.isEmpty() ? error : message;
}

} // namespace

SettingsRunnersPage::SettingsRunnersPage(runners::Requester requester, QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)) {
    setObjectName(QStringLiteral("settingsWorkspaceRunners"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Runners"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(7);
    layout->addWidget(textLabel(
        tr("Runners build and test on other machines while your code stays on this computer. "
           "Jobs go only to trusted runners: runners that are paired with this computer."),
        QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(14);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    add_ = new ket::Button(tr("Add runner"), ket::ButtonVariant::Primary, this);
    add_->setObjectName(QStringLiteral("runnersAdd"));
    add_->setControlSize(ket::ControlSize::Small);
    add_->setAccessibleName(tr("Add a runner"));
    add_->setFocusPolicy(Qt::StrongFocus);
    toolbar->addWidget(add_);
    refresh_ = new ket::Button(tr("Refresh"), ket::ButtonVariant::Secondary, this);
    refresh_->setObjectName(QStringLiteral("runnersRefresh"));
    refresh_->setControlSize(ket::ControlSize::Small);
    refresh_->setAccessibleName(tr("Refresh the runner list"));
    refresh_->setFocusPolicy(Qt::StrongFocus);
    toolbar->addWidget(refresh_);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);
    layout->addSpacing(10);

    error_ = new ket::Banner(ket::BannerTone::Danger, tr("Something went wrong"), {}, this);
    error_->setObjectName(QStringLiteral("runnersError"));
    error_->hide();
    layout->addWidget(error_);
    pinWarning_ =
        new ket::Banner(ket::BannerTone::Warning, tr("Some runner keys could not be read"),
                        tr("Runners whose key this computer could not read are not "
                           "trusted, and jobs do not go to them."),
                        this);
    pinWarning_->setObjectName(QStringLiteral("runnersPinWarning"));
    pinWarning_->hide();
    layout->addWidget(pinWarning_);

    empty_ = new ket::EmptyState({}, {}, {}, this);
    empty_->setObjectName(QStringLiteral("runnersEmpty"));
    empty_->setAction(tr("Refresh"));
    empty_->action()->setFocusPolicy(Qt::StrongFocus);
    empty_->hide();
    layout->addWidget(empty_);

    listBox_ = new QWidget(this);
    listBox_->setObjectName(QStringLiteral("runnersList"));
    listLayout_ = new QVBoxLayout(listBox_);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(0);
    listBox_->hide();
    layout->addWidget(listBox_);

    note_ = textLabel({}, QStringLiteral("settingsWorkspaceHelp"), this);
    note_->hide();
    layout->addSpacing(8);
    layout->addWidget(note_);
    layout->addStretch(1);

    connect(add_, &ket::Button::clicked, this, [this] { openPairing({}, {}); });
    connect(refresh_, &ket::Button::clicked, this, &SettingsRunnersPage::refresh);
    connect(empty_->action(), &ket::Button::clicked, this, [this] {
        switch (emptyAction_) {
        case EmptyAction::Account:
            emit accountRequested();
            break;
        case EmptyAction::Add:
            openPairing({}, {});
            break;
        case EmptyAction::Refresh:
            refresh();
            break;
        case EmptyAction::None:
            break;
        }
    });
}

SettingsRunnersPage::~SettingsRunnersPage() {
    if (confirm_)
        confirm_->deleteLater();
    // Rejects an open pairing and drops its token.
    delete pairing_.data();
}

QList<runners::RunnerRowWidget*> SettingsRunnersPage::rows() const {
    return listBox_->findChildren<runners::RunnerRowWidget*>(Qt::FindDirectChildrenOnly);
}

runners::RunnerRowWidget* SettingsRunnersPage::row(const QString& runnerId) const {
    for (runners::RunnerRowWidget* row : rows())
        if (row->runnerId() == runnerId)
            return row;
    return nullptr;
}

QString SettingsRunnersPage::error() const {
    return error_->isVisible() ? error_->message() : QString{};
}

QString SettingsRunnersPage::note() const { return note_->isVisible() ? note_->text() : QString{}; }

void SettingsRunnersPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

void SettingsRunnersPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // Minimizing the window hides it spontaneously; the pairing stays open meanwhile.
    if (!event->spontaneous())
        closeDialogs();
}

void SettingsRunnersPage::closeDialogs() {
    if (confirm_)
        confirm_->reject();
    if (pairing_)
        pairing_->close();
}

void SettingsRunnersPage::refresh() {
    if (!requester_)
        return;
    if (listing_) {
        queued_ = true;
        return;
    }
    listing_ = true;
    refresh_->setEnabled(false);
    const QPointer<SettingsRunnersPage> alive(this);
    requester_(QStringLiteral("runner.list"), {},
               [alive](const QJsonObject& result, const QString& error) {
                   if (alive)
                       alive->receiveList(result, error);
               });
}

void SettingsRunnersPage::receiveList(const QJsonObject& result, const QString& error) {
    const runners::RunnerList list = runners::parseRunnerList(result, error);
    if (list.unavailable == Unavailable::RelayUnavailable) {
        // Signed out and a Relay without runners give the same code; only the sign-in state
        // tells them apart.
        const QPointer<SettingsRunnersPage> alive(this);
        requester_(QStringLiteral("remote.status"), {},
                   [alive, list](const QJsonObject& status, const QString& statusError) {
                       if (alive)
                           alive->receiveStatus(list, status, statusError);
                   });
        return;
    }
    showList(list);
}

void SettingsRunnersPage::receiveStatus(const runners::RunnerList& list, const QJsonObject& status,
                                        const QString& error) {
    runners::RunnerList read = list;
    read.unavailable = runners::unavailableAfterStatus(status, error);
    showList(read);
}

void SettingsRunnersPage::showList(const runners::RunnerList& list) {
    listing_ = false;
    refresh_->setEnabled(true);
    list_ = list;
    if (std::exchange(queued_, false)) {
        refresh();
        return;
    }
    // A reply can arrive while a row is still in its own click: rows leave the list now and
    // are deleted later.
    for (QWidget* old : listBox_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->setParent(nullptr);
        old->deleteLater();
    }
    pinWarning_->setVisible(list.ok() && list.pinError);
    note_->hide();
    const QString actionError = std::exchange(actionError_, {});
    if (list.unavailable != Unavailable::None) {
        showError({});
        showUnavailable(list.unavailable);
        return;
    }
    add_->setEnabled(true);
    if (!list.errorCode.isEmpty()) {
        listBox_->hide();
        empty_->hide();
        showError(runners::errorText(list.errorCode, list.errorMessage));
        return;
    }
    showError(actionError);
    if (list.rows.isEmpty()) {
        listBox_->hide();
        empty_->setIconCode(QStringLiteral("terminal"));
        empty_->setTitle(tr("No runners yet"));
        empty_->setMessage(tr("Add a runner to build and test on another machine."));
        empty_->setAction(tr("Add runner"));
        empty_->action()->setVisible(true);
        emptyAction_ = EmptyAction::Add;
        empty_->show();
    } else {
        empty_->hide();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (int index = 0; index < list.rows.size(); ++index) {
            if (index > 0)
                listLayout_->addWidget(settings_rows::divider(listBox_));
            auto* row = new runners::RunnerRowWidget(list.rows.at(index), now, listBox_);
            row->setBusy(busy_.contains(row->runnerId()));
            connect(row, &runners::RunnerRowWidget::actionRequested, this,
                    &SettingsRunnersPage::onAction);
            listLayout_->addWidget(row);
        }
        listBox_->show();
    }
    QStringList notes;
    if (!list.complete)
        notes << tr("KetPlus Relay listed only part of your runners. Refresh to try again.");
    if (!list.unlistedPins.isEmpty())
        notes << runners::counted(
            int(list.unlistedPins.size()),
            QT_TRANSLATE_NOOP("SettingsRunnersPage",
                              "This computer still holds the key of 1 runner that your account "
                              "no longer lists. Jobs do not go to it."),
            QT_TRANSLATE_NOOP("SettingsRunnersPage",
                              "This computer still holds the keys of %1 runners that your "
                              "account no longer lists. Jobs do not go to them."));
    note_->setText(notes.join(QLatin1Char(' ')));
    note_->setVisible(!notes.isEmpty());
}

void SettingsRunnersPage::showUnavailable(const Unavailable unavailable) {
    listBox_->hide();
    add_->setEnabled(false);
    empty_->setIconCode(iconFor(unavailable));
    empty_->setTitle(runners::unavailableTitle(unavailable));
    empty_->setMessage(runners::unavailableMessage(unavailable));
    if (unavailable == Unavailable::NotEnrolled) {
        empty_->setAction(tr("Open Account & devices"));
        emptyAction_ = EmptyAction::Account;
    } else {
        empty_->setAction(tr("Refresh"));
        emptyAction_ = EmptyAction::Refresh;
    }
    empty_->action()->setVisible(true);
    empty_->show();
}

void SettingsRunnersPage::showError(const QString& message) {
    error_->setMessage(message);
    error_->setVisible(!message.isEmpty());
}

void SettingsRunnersPage::onAction(const RowAction action, const QString& runnerId) {
    if (busy_.contains(runnerId))
        return;
    switch (action) {
    case RowAction::Pair:
        if (runners::RunnerRowWidget* row = this->row(runnerId))
            openPairing(runnerId, row->displayName());
        return;
    case RowAction::Disable:
        send(QStringLiteral("runner.disable"), runnerId);
        return;
    case RowAction::Enable:
        send(QStringLiteral("runner.enable"), runnerId);
        return;
    case RowAction::Unpair:
    case RowAction::Remove:
        confirm(action, runnerId);
        return;
    }
}

void SettingsRunnersPage::confirm(const RowAction action, const QString& runnerId) {
    runners::RunnerRowWidget* row = this->row(runnerId);
    if (!row || confirm_ || pairing_)
        return;
    const QString name = row->displayName();
    const bool removing = action == RowAction::Remove;
    auto* dialog = new ket::Dialog(removing ? tr("Remove %1 from your account?").arg(name)
                                            : tr("Unpair %1 from this computer?").arg(name),
                                   this);
    dialog->setObjectName(QStringLiteral("runnersConfirm"));
    dialog->setCardWidth(440);
    auto* body = new QWidget(dialog);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* text = runners::dialog_text::make(
        removing ? tr("%1 is removed from your KetPlus account and from every computer it is "
                      "paired with, and no job goes to it again. To use that node again, "
                      "register it again.")
                       .arg(name)
                 : tr("This computer stops trusting %1 and sends it no more jobs. The runner "
                      "stays in your account; pair it again to use it.")
                       .arg(name),
        runners::dialog_text::Role::Label, body);
    text->setObjectName(QStringLiteral("runnersConfirmText"));
    layout->addWidget(text);
    dialog->setBody(body);
    runners::dialog_text::follow(body);
    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Secondary);
    cancel->setObjectName(QStringLiteral("runnersConfirmCancel"));
    auto* go =
        dialog->addButton(removing ? tr("Remove") : tr("Unpair"), ket::ButtonVariant::Danger);
    go->setObjectName(QStringLiteral("runnersConfirmAction"));
    go->setAccessibleName(removing ? tr("Remove %1").arg(name) : tr("Unpair %1").arg(name));
    connect(cancel, &ket::Button::clicked, dialog, &ket::Dialog::reject);
    connect(go, &ket::Button::clicked, dialog, &ket::Dialog::accept);
    const QString type =
        removing ? QStringLiteral("runner.remove") : QStringLiteral("runner.unpair");
    connect(dialog, &ket::Dialog::finished, this, [this, dialog, type, runnerId](bool accepted) {
        dialog->deleteLater();
        if (accepted)
            send(type, runnerId);
    });
    confirm_ = dialog;
    dialog->open();
}

void SettingsRunnersPage::send(const QString& type, const QString& runnerId) {
    if (!requester_ || busy_.contains(runnerId))
        return;
    busy_.insert(runnerId);
    if (runners::RunnerRowWidget* row = this->row(runnerId))
        row->setBusy(true);
    showError({});
    const QPointer<SettingsRunnersPage> alive(this);
    requester_(type, {{QStringLiteral("runner_id"), runnerId}},
               [alive, runnerId](const QJsonObject& result, const QString& error) {
                   if (!alive)
                       return;
                   alive->busy_.remove(runnerId);
                   if (runners::RunnerRowWidget* row = alive->row(runnerId))
                       row->setBusy(false);
                   const QString code = failureCode(result);
                   if (!error.isEmpty() || !code.isEmpty()) {
                       alive->actionError_ =
                           runners::errorText(code, failureMessage(result, error));
                       alive->showError(alive->actionError_);
                   }
                   alive->refresh();
               });
}

void SettingsRunnersPage::openPairing(const QString& runnerId, const QString& runnerName) {
    if (pairing_ || confirm_ || !requester_)
        return;
    auto* pairing = new runners::RunnerPairingDialog(requester_, this, runnerId, runnerName, this);
    pairing_ = pairing;
    connect(pairing, &runners::RunnerPairingDialog::closed, this, [this, pairing](bool) {
        pairing->deleteLater();
        refresh();
    });
    pairing->start();
}

void SettingsRunnersPage::pairingChanged(const QJsonObject& payload) {
    if (pairing_)
        pairing_->pairingChanged(payload);
    if (isVisible())
        refresh();
}

} // namespace ketplus
