#include "remote/LinuxMachinesSection.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetTextField.h"

#include "KetPlusProductVersion.h"
#include "control/ControlV2Client.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <memory>

namespace ketplus {

LinuxMachinesSection::LinuxMachinesSection(QWidget* parent)
    : QWidget(parent), client_(new remote::ControlV2Client(
                           QUrl(QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL)), this)) {
    setObjectName(QStringLiteral("linuxMachinesSection"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 6, 0, 0);
    outer->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Linux machines"), this);
    title->setObjectName(QStringLiteral("linuxMachinesTitle"));
    heading->addWidget(title);
    count_ = new QLabel(tr("0 available"), this);
    count_->setObjectName(QStringLiteral("linuxMachinesCount"));
    heading->addWidget(count_);
    heading->addStretch();
    add_ = new ket::Button(tr("Connect Linux machine"), ket::ButtonVariant::Secondary, this);
    add_->setObjectName(QStringLiteral("linuxMachinesAdd"));
    connect(add_, &QAbstractButton::clicked, this, &LinuxMachinesSection::startPairing);
    heading->addWidget(add_);
    outer->addLayout(heading);
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("linuxMachinesNotice"));
    notice_->setWordWrap(true);
    notice_->hide();
    outer->addWidget(notice_);
    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("linuxMachinesSurface"));
    surface->setProperty("remoteRole", QStringLiteral("surface"));
    rows_ = new QVBoxLayout(surface);
    rows_->setContentsMargins(12, 12, 12, 12);
    rows_->setSpacing(8);
    outer->addWidget(surface);
    auto* footnote =
        new QLabel(tr("Linux machines connect through an authenticated outbound relay. "
                      "Compare the identity shown here with ketplusctl on Linux."),
                   this);
    footnote->setObjectName(QStringLiteral("linuxMachinesFootnote"));
    footnote->setWordWrap(true);
    outer->addWidget(footnote);
    connect(client_, &remote::ControlV2Client::signInFinished, this, [this](const QString& error) {
        setNotice(error.isEmpty() ? tr("Signed in. You can pair a Linux machine.") : error,
                  !error.isEmpty());
        render();
        if (error.isEmpty())
            refreshMachines();
    });
    render();
}

QStringList LinuxMachinesSection::machineIds() const { return machineIds_; }

void LinuxMachinesSection::activate() {
    render();
    if (client_->signedIn())
        refreshMachines();
}

void LinuxMachinesSection::setNotice(const QString& message, const bool error) {
    notice_->setText(message);
    notice_->setProperty("remoteTone",
                         error ? QStringLiteral("danger") : QStringLiteral("positive"));
    notice_->setVisible(!message.isEmpty());
    notice_->style()->unpolish(notice_);
    notice_->style()->polish(notice_);
}

void LinuxMachinesSection::render() {
    while (auto* item = rows_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    count_->setText(tr("%1 available").arg(machineIds_.size()));
    add_->setVisible(client_->signedIn());
    if (!client_->signedIn()) {
        auto* description = new QLabel(tr("Sign in with the same KetSuite account used by the "
                                          "headless Linux daemon."),
                                       this);
        description->setWordWrap(true);
        rows_->addWidget(description);
        auto* signInButton = new ket::Button(tr("Sign in to connect a Linux machine"),
                                             ket::ButtonVariant::Primary, this);
        connect(signInButton, &QAbstractButton::clicked, this, &LinuxMachinesSection::signIn);
        rows_->addWidget(signInButton, 0, Qt::AlignLeft);
        return;
    }
    if (loading_) {
        rows_->addWidget(new QLabel(tr("Loading trusted machines…"), this));
        return;
    }
    if (machineIds_.isEmpty()) {
        auto* empty = new QLabel(tr("No Linux machines yet. Start pairing on the Linux daemon, "
                                    "then paste its one-time pairing link here."),
                                 this);
        empty->setWordWrap(true);
        rows_->addWidget(empty);
        return;
    }
    for (const auto& id : machineIds_) {
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("linuxMachineRow"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(6, 8, 6, 8);
        auto* name = new QLabel(id, row);
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(name, 1);
        auto* status = new QLabel(tr("Available"), row);
        status->setAccessibleName(tr("Machine %1 is available").arg(id));
        layout->addWidget(status);
        rows_->addWidget(row);
    }
}

void LinuxMachinesSection::signIn() {
    setNotice(tr("Opening KetSuite sign-in…"));
    client_->startSignIn([this](const QJsonValue& value, const QString& error) {
        if (!error.isEmpty()) {
            setNotice(error, true);
            return;
        }
        const QUrl page(value.toObject().value(QStringLiteral("authorizationUrl")).toString());
        if (!QDesktopServices::openUrl(page)) {
            client_->cancelSignIn();
            setNotice(tr("Could not open the account sign-in page."), true);
            return;
        }
        setNotice(tr("Finish sign-in in your browser, then return here."));
    });
}

void LinuxMachinesSection::startPairing() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Pair a Linux machine"));
    dialog->setModal(true);
    dialog->setMinimumWidth(470);
    auto* layout = new QVBoxLayout(dialog);
    auto* instructions = new QLabel(tr("Run ketplusctl pairing create on the Linux machine, "
                                       "then paste the full ketplus:// pairing link."),
                                    dialog);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    auto* input = new ket::TextField(dialog);
    input->setObjectName(QStringLiteral("linuxPairingLink"));
    input->setAccessibleName(tr("One-time Linux pairing link"));
    input->setEchoMode(QLineEdit::Password);
    input->setPlaceholderText(QStringLiteral("ketplus://pair?ticket=…&secret=…"));
    layout->addWidget(input);
    auto* feedback = new QLabel(dialog);
    feedback->setObjectName(QStringLiteral("linuxPairingFeedback"));
    feedback->setWordWrap(true);
    layout->addWidget(feedback);
    auto* match = new QCheckBox(tr("I checked the identity on both machines."), dialog);
    match->hide();
    layout->addWidget(match);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto* check = buttons->addButton(tr("Check link"), QDialogButtonBox::ActionRole);
    auto* approve = buttons->addButton(tr("Check approval"), QDialogButtonBox::AcceptRole);
    approve->setEnabled(false);
    layout->addWidget(buttons);
    auto ticketId = std::make_shared<QString>();
    const QPointer<QDialog> dialogGuard(dialog);
    connect(dialog, &QObject::destroyed, this, [this] { client_->cancelPairing(); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, [this, dialog] {
        client_->cancelPairing();
        dialog->reject();
    });
    connect(match, &QCheckBox::toggled, approve, &QAbstractButton::setEnabled);
    connect(
        check, &QAbstractButton::clicked, dialog,
        [this, input, feedback, match, check, ticketId, dialogGuard] {
            const QUrl link(input->text().trimmed());
            input->clear();
            const QUrlQuery query(link);
            const auto ticket = query.queryItemValue(QStringLiteral("ticket"));
            const auto secret = query.queryItemValue(QStringLiteral("secret"));
            if (link.scheme() != QStringLiteral("ketplus") ||
                link.host() != QStringLiteral("pair") || ticket.isEmpty() || secret.size() < 32) {
                feedback->setText(tr("Paste the complete, unexpired pairing link from Linux."));
                input->setFocus();
                return;
            }
            check->setEnabled(false);
            feedback->setText(tr("Checking the Linux pairing request…"));
            // The link names the Linux host's keys; the client pins them (#88).
            const QJsonObject host{
                {QStringLiteral("machineId"),
                 query.queryItemValue(QStringLiteral("machine"), QUrl::FullyDecoded)},
                {QStringLiteral("keyVersion"), query.queryItemValue(QStringLiteral("kv")).toInt()},
                {QStringLiteral("fingerprint"), query.queryItemValue(QStringLiteral("fp"))}};
            client_->claimPairing(
                ticket, secret, QStringLiteral("KetPlus Desktop"), host,
                [feedback, match, check, ticketId, ticket, dialogGuard](const QJsonValue& value,
                                                                        const QString& error) {
                    if (!dialogGuard)
                        return;
                    check->setEnabled(true);
                    if (!error.isEmpty()) {
                        feedback->setText(error);
                        return;
                    }
                    *ticketId = ticket;
                    const auto digest =
                        value.toObject().value(QStringLiteral("claimDigest")).toString();
                    feedback->setText(QObject::tr("Compare the first six characters of claimDigest "
                                                  "shown by ketplusctl pairing status: %1")
                                          .arg(digest.left(6).toUpper()));
                    match->show();
                });
        });
    connect(approve, &QAbstractButton::clicked, dialog,
            [this, dialog, feedback, approve, ticketId, dialogGuard] {
                approve->setEnabled(false);
                client_->pairingStatus(*ticketId, [this, dialog, feedback, approve,
                                                   dialogGuard](const QJsonValue& value,
                                                                const QString& error) {
                    if (!dialogGuard)
                        return;
                    if (!error.isEmpty()) {
                        feedback->setText(error);
                        approve->setEnabled(true);
                        return;
                    }
                    const auto ticket = value.toObject();
                    if (ticket.value(QStringLiteral("status")) != QStringLiteral("approved")) {
                        feedback->setText(tr("Approve the request on Linux, then check again."));
                        approve->setEnabled(true);
                        return;
                    }
                    QString failure;
                    if (!client_->acceptPairing(ticket, &failure)) {
                        feedback->setText(failure);
                        approve->setEnabled(true);
                        return;
                    }
                    setNotice(tr("Linux machine trusted."));
                    dialog->accept();
                    refreshMachines();
                });
            });
    dialog->show();
    input->setFocus();
}

void LinuxMachinesSection::beginPairing() {
    if (client_->signedIn())
        startPairing();
    else
        signIn();
}

void LinuxMachinesSection::refreshMachines() {
    if (loading_ || !client_->signedIn())
        return;
    loading_ = true;
    render();
    client_->listWorktrees([this](const QJsonValue& value, const QString& error) {
        loading_ = false;
        if (!error.isEmpty()) {
            setNotice(error, true);
            render();
            return;
        }
        QStringList ids;
        QSet<QString> seen;
        for (const auto& item : value.toArray()) {
            const auto id = item.toObject().value(QStringLiteral("machineId")).toString();
            if (!id.isEmpty() && !seen.contains(id)) {
                seen.insert(id);
                ids.append(id);
            }
        }
        machineIds_ = ids;
        render();
        emit machinesChanged(ids, ids);
    });
}

} // namespace ketplus
