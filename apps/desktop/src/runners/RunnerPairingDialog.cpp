#include "runners/RunnerPairingDialog.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetTextField.h"
#include "KetToast.h"
#include "runners/RunnerDialogText.h"
#include "runners/RunnerMessages.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

#include <limits>
#include <utility>

namespace ketplus::runners {
namespace {

using dialog_text::Role;

QString failureCode(const QJsonObject& result) {
    return result.value(QStringLiteral("error_code")).toString();
}

QString failureMessage(const QJsonObject& result, const QString& error) {
    const QString message = result.value(QStringLiteral("error_message")).toString();
    return message.isEmpty() ? error : message;
}

bool failed(const QJsonObject& result, const QString& error) {
    return !error.isEmpty() || !failureCode(result).isEmpty() ||
           result.value(QStringLiteral("ok")) == QJsonValue(false);
}

std::optional<int> attemptsIn(const QJsonObject& result) {
    const QJsonValue value = result.value(QStringLiteral("attempts_left"));
    if (!value.isDouble())
        return std::nullopt;
    return value.toInt();
}

} // namespace

RunnerPairingDialog::RunnerPairingDialog(Requester requester, QWidget* anchor,
                                         const QString& runnerId, const QString& runnerName,
                                         QObject* parent)
    : QObject(parent), requester_(std::move(requester)), runnerId_(runnerId),
      newRunner_(runnerId.isEmpty()) {
    dialog_ = new ket::Dialog(
        newRunner_ ? tr("Add a runner") : tr("Pair %1 with this computer").arg(runnerName), anchor);
    dialog_->setObjectName(QStringLiteral("runnerPairingDialog"));
    dialog_->setCardWidth(480);

    auto* body = new QWidget(dialog_);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    message_ = dialog_text::make(tr("Creating a pairing token…"), Role::Label, body);
    message_->setObjectName(QStringLiteral("runnerPairingMessage"));
    layout->addWidget(message_);

    tokenBox_ = new QWidget(body);
    auto* tokenLayout = new QVBoxLayout(tokenBox_);
    tokenLayout->setContentsMargins(0, 0, 0, 0);
    tokenLayout->setSpacing(6);
    tokenLayout->addWidget(dialog_text::make(
        newRunner_ ? tr("On the node, run this command and paste the token when it asks:")
                   : tr("On the runner's node, run this command and paste the token when it "
                        "asks:"),
        Role::Help, tokenBox_));
    auto* commandRow = new QHBoxLayout;
    command_ = dialog_text::make(nodeCommand(newRunner_), Role::Label, tokenBox_);
    command_->setObjectName(QStringLiteral("runnerPairingCommand"));
    command_->setFont(QFont(QStringLiteral("monospace")));
    commandRow->addWidget(command_, 1);
    copyCommand_ = new ket::Button(tr("Copy command"), ket::ButtonVariant::Secondary, tokenBox_);
    copyCommand_->setControlSize(ket::ControlSize::Small);
    copyCommand_->setObjectName(QStringLiteral("runnerCopyCommand"));
    commandRow->addWidget(copyCommand_);
    tokenLayout->addLayout(commandRow);
    auto* tokenRow = new QHBoxLayout;
    token_ = new ket::TextField(tokenBox_);
    token_->setObjectName(QStringLiteral("runnerPairingToken"));
    token_->setEchoMode(QLineEdit::Password);
    token_->setReadOnly(true);
    token_->setContextMenuPolicy(Qt::NoContextMenu);
    token_->setAccessibleName(tr("Pairing token, hidden"));
    tokenRow->addWidget(token_, 1);
    copyToken_ = new ket::Button(tr("Copy token"), ket::ButtonVariant::Secondary, tokenBox_);
    copyToken_->setControlSize(ket::ControlSize::Small);
    copyToken_->setObjectName(QStringLiteral("runnerCopyToken"));
    copyToken_->setEnabled(false);
    tokenRow->addWidget(copyToken_);
    tokenLayout->addLayout(tokenRow);
    expires_ = dialog_text::make({}, Role::Help, tokenBox_);
    expires_->setObjectName(QStringLiteral("runnerPairingExpiry"));
    tokenLayout->addWidget(expires_);
    tokenLayout->addWidget(
        dialog_text::make(tr("The token is secret and works once. It is not saved anywhere."),
                          Role::Help, tokenBox_));
    tokenBox_->hide();
    layout->addWidget(tokenBox_);

    runnerBox_ = new QWidget(body);
    auto* runnerLayout = new QVBoxLayout(runnerBox_);
    runnerLayout->setContentsMargins(0, 0, 0, 0);
    runnerLayout->setSpacing(4);
    runnerName_ = dialog_text::make({}, Role::Label, runnerBox_);
    runnerName_->setObjectName(QStringLiteral("runnerPairingName"));
    runnerLayout->addWidget(runnerName_);
    runnerDetail_ = dialog_text::make({}, Role::Help, runnerBox_);
    runnerLayout->addWidget(runnerDetail_);
    fingerprint_ = dialog_text::make({}, Role::Help, runnerBox_);
    runnerLayout->addWidget(fingerprint_);
    runnerLayout->addSpacing(6);
    runnerLayout->addWidget(dialog_text::make(tr("Confirmation code"), Role::Help, runnerBox_));
    code_ = dialog_text::make({}, Role::Code, runnerBox_);
    code_->setObjectName(QStringLiteral("runnerPairingCode"));
    code_->setAccessibleName(tr("Confirmation code"));
    runnerLayout->addWidget(code_);
    runnerBox_->hide();
    layout->addWidget(runnerBox_);

    error_ = dialog_text::make({}, Role::Error, body);
    error_->setObjectName(QStringLiteral("runnerPairingError"));
    error_->hide();
    layout->addWidget(error_);
    dialog_->setBody(body);
    dialog_text::follow(body);

    check_ = dialog_->addButton(tr("Check again"), ket::ButtonVariant::Ghost);
    check_->setObjectName(QStringLiteral("runnerPairingCheck"));
    check_->hide();
    reject_ = dialog_->addButton(tr("Cancel"), ket::ButtonVariant::Secondary);
    reject_->setObjectName(QStringLiteral("runnerPairingReject"));
    approve_ = dialog_->addButton(tr("Approve"), ket::ButtonVariant::Primary);
    approve_->setObjectName(QStringLiteral("runnerPairingApprove"));
    approve_->setAccessibleName(tr("Approve the runner"));
    approve_->hide();
    approve_->setEnabled(false);

    expiry_.setSingleShot(true);
    connect(&expiry_, &QTimer::timeout, this, &RunnerPairingDialog::expire);
    connect(check_, &ket::Button::clicked, this, &RunnerPairingDialog::readStatus);
    connect(reject_, &ket::Button::clicked, this, &RunnerPairingDialog::rejectClicked);
    connect(approve_, &ket::Button::clicked, this, &RunnerPairingDialog::approveClicked);
    connect(copyToken_, &ket::Button::clicked, this, [this] {
        if (token_->text().isEmpty())
            return;
        QGuiApplication::clipboard()->setText(token_->text());
        ket::Toast::showMessage(dialog_, tr("Token copied"));
    });
    connect(copyCommand_, &ket::Button::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(nodeCommand(newRunner_));
        ket::Toast::showMessage(dialog_, tr("Command copied"));
    });
    connect(dialog_, &ket::Dialog::finished, this, &RunnerPairingDialog::onDialogFinished);
}

RunnerPairingDialog::~RunnerPairingDialog() {
    if (dialog_)
        disconnect(dialog_, nullptr, this, nullptr);
    if (!closed_) {
        closed_ = true;
        if (!finished_)
            sendReject();
    }
    // The window may have taken the dialog and its fields with it.
    if (dialog_)
        clearToken();
    delete dialog_.data();
}

QString RunnerPairingDialog::message() const { return message_->text(); }

QString RunnerPairingDialog::error() const {
    return error_->isVisible() ? error_->text() : QString{};
}

void RunnerPairingDialog::start() {
    if (creating_ || view_ || closed_ || !requester_)
        return;
    creating_ = true;
    dialog_->open();
    QJsonObject payload;
    if (!newRunner_)
        payload.insert(QStringLiteral("runner_id"), runnerId_);
    const QPointer<RunnerPairingDialog> alive(this);
    const Requester requester = requester_;
    requester_(QStringLiteral("runner.pairing.create"), payload,
               [alive, requester](const QJsonObject& result, const QString& error) {
                   if (alive && !alive->closed_) {
                       alive->receiveCreate(result, error);
                       return;
                   }
                   // Closed before the token arrived: the token is dropped unseen and the
                   // pairing it opened is rejected.
                   const QString ticket = result.value(QStringLiteral("ticket_id")).toString();
                   if (!failed(result, error) && !ticket.isEmpty() && requester)
                       requester(QStringLiteral("runner.pairing.reject"),
                                 {{QStringLiteral("ticket_id"), ticket}},
                                 [](const QJsonObject&, const QString&) {});
               });
}

void RunnerPairingDialog::receiveCreate(const QJsonObject& result, const QString& error) {
    creating_ = false;
    if (failed(result, error)) {
        finished_ = true;
        message_->setText(tr("No pairing was started."));
        showError(errorText(failureCode(result), failureMessage(result, error)));
        reject_->setText(tr("Close"));
        return;
    }
    const std::optional<PairingView> view = parsePairing(result);
    const QString token = pairingToken(result);
    if (!view || token.isEmpty()) {
        if (view) {
            view_ = view;
            sendReject();
        }
        finished_ = true;
        message_->setText(tr("No pairing was started."));
        showError(errorText(QStringLiteral("invalid_response"), {}));
        reject_->setText(tr("Close"));
        return;
    }
    token_->setText(token);
    copyToken_->setEnabled(true);
    tokenBox_->show();
    if (view->expiresAt) {
        const qint64 left = QDateTime::currentDateTimeUtc().msecsTo(*view->expiresAt);
        expiry_.start(int(qBound<qint64>(0, left, qint64(std::numeric_limits<int>::max()))));
        expires_->setText(tr("The token expires at %1.")
                              .arg(QLocale().toString(view->expiresAt->toLocalTime().time(),
                                                      QLocale::ShortFormat)));
    }
    show(*view);
}

void RunnerPairingDialog::pairingChanged(const QJsonObject& payload) {
    if (!view_ || closed_ || finished_)
        return;
    if (payload.value(QStringLiteral("ticket_id")).toString() != view_->ticketId)
        return;
    readStatus();
}

void RunnerPairingDialog::readStatus() {
    if (!view_ || reading_ || closed_ || !requester_)
        return;
    reading_ = true;
    const QPointer<RunnerPairingDialog> alive(this);
    requester_(QStringLiteral("runner.pairing.status"),
               {{QStringLiteral("ticket_id"), view_->ticketId}},
               [alive](const QJsonObject& result, const QString& error) {
                   if (!alive || alive->closed_)
                       return;
                   alive->reading_ = false;
                   alive->receivePairing(result, error, false);
               });
}

void RunnerPairingDialog::approveClicked() {
    if (!view_ || !view_->canApprove() || approving_ || rejectSent_ || closed_ || !requester_)
        return;
    approving_ = true;
    approve_->setEnabled(false);
    showError({});
    const QPointer<RunnerPairingDialog> alive(this);
    requester_(QStringLiteral("runner.pairing.approve"),
               {{QStringLiteral("ticket_id"), view_->ticketId},
                {QStringLiteral("code"), view_->confirmationCode}},
               [alive](const QJsonObject& result, const QString& error) {
                   if (!alive || alive->closed_)
                       return;
                   alive->approving_ = false;
                   alive->receivePairing(result, error, true);
               });
}

void RunnerPairingDialog::rejectClicked() {
    if (finished_ || !view_) {
        // Nothing open to reject: while the token is still being made, closing rejects it
        // once it arrives.
        dialog_->reject();
        return;
    }
    showError({});
    sendReject();
}

void RunnerPairingDialog::receivePairing(const QJsonObject& result, const QString& error,
                                         const bool fromApprove) {
    if (failed(result, error)) {
        const QString code = failureCode(result);
        showError(pairingErrorText(code, attemptsIn(result), failureMessage(result, error)));
        if (code == QStringLiteral("ticket_not_found") && view_) {
            view_->stage = PairingView::Stage::Failed;
            show(*view_);
            return;
        }
        if (view_)
            show(*view_);
        // The attempts left and the stage after a refused approval come from the daemon.
        if (fromApprove)
            readStatus();
        return;
    }
    if (const std::optional<PairingView> view = parsePairing(result))
        show(*view);
}

void RunnerPairingDialog::show(const PairingView& view) {
    if (view_ && view.ticketId != view_->ticketId)
        return;
    using Stage = PairingView::Stage;
    view_ = view;
    message_->setText(stageText(view));
    if (view.stage != Stage::Waiting) {
        // A claimed token cannot be used again: it leaves the screen.
        clearToken();
        tokenBox_->hide();
    }
    const bool claimed = view.stage == Stage::Confirm || view.stage == Stage::Approving ||
                         view.stage == Stage::Paired;
    runnerBox_->setVisible(claimed && !view.runnerId.isEmpty());
    if (claimed) {
        const QString name = plain(view.runnerName);
        runnerName_->setText(name.isEmpty() ? view.runnerId : name);
        QStringList detail;
        const QString platform = QStringList{plain(view.runnerOs, 24), plain(view.runnerArch, 24)}
                                     .join(QLatin1Char(' '))
                                     .trimmed();
        if (!platform.isEmpty())
            detail << platform;
        QStringList labels;
        for (const QString& label : view.runnerLabels)
            labels << plain(label, 32);
        if (!labels.isEmpty())
            detail << tr("Labels: %1").arg(labels.join(QStringLiteral(", ")));
        runnerDetail_->setText(detail.join(QStringLiteral(" · ")));
        runnerDetail_->setVisible(!detail.isEmpty());
        const QString fingerprint = plain(view.fingerprint, 120);
        fingerprint_->setText(tr("Key fingerprint: %1").arg(fingerprint));
        fingerprint_->setVisible(!fingerprint.isEmpty());
    }
    code_->setText(plain(view.confirmationCode, 32));
    code_->setVisible(!view.confirmationCode.isEmpty());
    if (!view.lastError.isEmpty() && !view.finished())
        showError(pairingErrorText(view.lastError, view.attemptsLeft, {}));

    if (view.finished()) {
        finished_ = true;
        expiry_.stop();
        clearToken();
        tokenBox_->hide();
        approve_->hide();
        check_->hide();
        reject_->setText(tr("Close"));
        reject_->setAccessibleName(tr("Close"));
        return;
    }
    const bool confirming = view.stage == Stage::Confirm || view.stage == Stage::Approving;
    approve_->setVisible(confirming);
    approve_->setEnabled(view.canApprove() && !approving_ && !rejectSent_);
    check_->setVisible(view.stage == Stage::Waiting || view.stage == Stage::Approving);
    reject_->setText(confirming ? tr("Reject") : tr("Cancel"));
    reject_->setAccessibleName(confirming ? tr("Reject the runner") : tr("Cancel the pairing"));
    reject_->setEnabled(view.canReject() && !rejectSent_);
}

void RunnerPairingDialog::expire() {
    if (finished_)
        return;
    clearToken();
    tokenBox_->hide();
    showError(pairingErrorText(QStringLiteral("ticket_expired"), std::nullopt, {}));
    readStatus();
}

void RunnerPairingDialog::close() {
    if (dialog_ && dialog_->isOpen())
        dialog_->reject();
}

void RunnerPairingDialog::onDialogFinished() {
    if (closed_)
        return;
    closed_ = true;
    expiry_.stop();
    clearToken();
    if (!finished_)
        sendReject();
    emit closed(view_ && view_->stage == PairingView::Stage::Paired);
}

void RunnerPairingDialog::sendReject() {
    if (rejectSent_ || !view_ || !requester_)
        return;
    rejectSent_ = true;
    if (dialog_) {
        approve_->setEnabled(false);
        reject_->setEnabled(false);
    }
    const QPointer<RunnerPairingDialog> alive(this);
    requester_(QStringLiteral("runner.pairing.reject"),
               {{QStringLiteral("ticket_id"), view_->ticketId}},
               [alive](const QJsonObject& result, const QString& error) {
                   if (!alive || alive->closed_)
                       return;
                   alive->reject_->setEnabled(true);
                   if (failed(result, error)) {
                       // The pairing may have moved on (approving, or already ended): its
                       // state comes from the daemon.
                       alive->rejectSent_ = false;
                       alive->showError(pairingErrorText(failureCode(result), std::nullopt,
                                                         failureMessage(result, error)));
                       alive->readStatus();
                       return;
                   }
                   std::optional<PairingView> view = parsePairing(result);
                   if (!view && alive->view_) {
                       view = alive->view_;
                       view->stage = PairingView::Stage::Rejected;
                   }
                   if (view)
                       alive->show(*view);
               });
}

void RunnerPairingDialog::showError(const QString& text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

void RunnerPairingDialog::clearToken() {
    token_->clear();
    copyToken_->setEnabled(false);
}

} // namespace ketplus::runners
