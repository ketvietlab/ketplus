#include "remote/LinuxMachinesSection.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "KetTokens.h"

#include "KetPlusProductVersion.h"
#include "control/ControlV2Client.h"

#include <QCheckBox>
#include <QDesktopServices>
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
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(18); // the page's rhythm
    // The mock's section: "Linux machines · N trusted"; the page's Add menu starts pairing.
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Linux machines"), this);
    title->setObjectName(QStringLiteral("linuxMachinesTitle"));
    title->setProperty("remoteRole", QStringLiteral("sectionTitle"));
    heading->addWidget(title);
    heading->addStretch();
    count_ = new QLabel(tr("0 trusted"), this);
    count_->setObjectName(QStringLiteral("linuxMachinesCount"));
    count_->setProperty("remoteRole", QStringLiteral("sectionCount"));
    heading->addWidget(count_);
    outer->addLayout(heading);
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("linuxMachinesNotice"));
    notice_->setWordWrap(true);
    notice_->hide();
    outer->addWidget(notice_);
    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("linuxMachinesSurface"));
    // One card for each machine, as the mock; a message is a card of its own.
    rows_ = new QVBoxLayout(surface);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(18);
    outer->addWidget(surface);
    auto* footnoteRow = new QHBoxLayout;
    footnoteRow->setSpacing(8);
    auto* shield = new QLabel(this);
    shield->setFixedSize(13, 13);
    const auto inkShield = [this, shield] {
        shield->setPixmap(ket::Icons::pixmap(
            QStringLiteral("shield"), 13, devicePixelRatioF(),
            ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme())));
    };
    inkShield();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, shield, inkShield);
    footnoteRow->addWidget(shield, 0, Qt::AlignTop);
    auto* footnote =
        new QLabel(tr("Linux machines connect through an authenticated outbound relay. "
                      "Compare the identity shown here with ketplusctl on Linux."),
                   this);
    footnote->setObjectName(QStringLiteral("linuxMachinesFootnote"));
    footnote->setProperty("remoteRole", QStringLiteral("trustNote"));
    footnote->setWordWrap(true);
    footnoteRow->addWidget(footnote, 1);
    outer->addLayout(footnoteRow);
    connect(client_, &remote::ControlV2Client::signInFinished, this, [this](const QString& error) {
        setNotice(error.isEmpty() ? tr("Signed in. You can pair a Linux machine.") : error,
                  !error.isEmpty());
        render();
        if (error.isEmpty())
            refreshMachines();
    });
    // The rows' icons are drawn in the theme's ink.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &LinuxMachinesSection::render);
    render();
}

QStringList LinuxMachinesSection::machineIds() const { return machineIds_; }

void LinuxMachinesSection::activate() {
    render();
    if (client_->signedIn())
        refreshMachines();
}

void LinuxMachinesSection::setPairingEntryShown(const bool shown) {
    if (pairingEntryShown_ == shown)
        return;
    pairingEntryShown_ = shown;
    render();
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
    count_->setText(tr("%1 trusted").arg(machineIds_.size()));
    const auto message = [this](const QString& text) {
        auto* box = new QFrame(this);
        box->setProperty("remoteRole", QStringLiteral("surface"));
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(22, 20, 22, 20);
        layout->setSpacing(12);
        auto* label = new QLabel(text, box);
        label->setProperty("remoteRole", QStringLiteral("emptyText"));
        label->setWordWrap(true);
        layout->addWidget(label);
        rows_->addWidget(box);
        return layout;
    };
    if (!client_->signedIn()) {
        auto* layout = message(tr("Sign in with the same KetSuite account used by the headless "
                                  "Linux daemon."));
        auto* signInButton = new ket::Button(tr("Sign in to connect a Linux machine"),
                                             ket::ButtonVariant::Primary, this);
        connect(signInButton, &QAbstractButton::clicked, this, &LinuxMachinesSection::signIn);
        layout->addWidget(signInButton, 0, Qt::AlignLeft);
        return;
    }
    if (loading_) {
        message(tr("Loading trusted machines…"));
        return;
    }
    const auto addConnectButton = [this](QBoxLayout* layout) {
        auto* connectButton =
            new ket::Button(tr("Connect a Linux machine"), ket::ButtonVariant::Secondary, this);
        connectButton->setObjectName(QStringLiteral("linuxMachinesConnect"));
        connect(connectButton, &QAbstractButton::clicked, this,
                &LinuxMachinesSection::beginPairing);
        layout->addWidget(connectButton, 0, Qt::AlignLeft);
    };
    if (machineIds_.isEmpty()) {
        if (pairingEntryShown_) {
            addConnectButton(message(tr("No Linux machines yet. Start pairing on the Linux "
                                        "daemon, then paste its one-time pairing link here.")));
        } else {
            message(tr("No Linux machines yet. Start pairing on the Linux daemon, then choose "
                       "Add › Connect a Linux machine and paste its one-time pairing link."));
        }
        return;
    }
    // A device row as the phones above: icon, name, how it connects, and its state.
    const auto theme = ket::Appearance::instance().theme();
    for (const auto& id : machineIds_) {
        auto* row = new QFrame(this);
        row->setObjectName(QStringLiteral("linuxMachineRow"));
        row->setProperty("remoteRole", QStringLiteral("deviceRow"));
        auto* layout = new QHBoxLayout(row);
        row->setFixedHeight(94);
        layout->setContentsMargins(22, 0, 22, 0);
        layout->setSpacing(18);
        auto* iconSurface = new QFrame(row);
        iconSurface->setProperty("remoteRole", QStringLiteral("deviceIconSurface"));
        iconSurface->setFixedSize(40, 40);
        auto* iconLayout = new QVBoxLayout(iconSurface);
        iconLayout->setContentsMargins(10, 10, 10, 10);
        auto* icon = new QLabel(iconSurface);
        icon->setPixmap(ket::Icons::pixmap(ket::Icons::codeForLucide(u"terminal"), 18,
                                           devicePixelRatioF(),
                                           ket::Tokens::color(u"text-secondary", theme)));
        iconLayout->addWidget(icon, 0, Qt::AlignCenter);
        layout->addWidget(iconSurface);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(6);
        copy->addStretch(1);
        // The name with its state and how it connects, as the mock's badges.
        auto* nameLine = new QHBoxLayout;
        nameLine->setSpacing(8);
        auto* name = new QLabel(id, row);
        name->setProperty("remoteRole", QStringLiteral("deviceName"));
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameLine->addWidget(name);
        auto* status = new QLabel(tr("Available"), row);
        status->setProperty("remoteRole", QStringLiteral("badge"));
        status->setProperty("remoteTone", QStringLiteral("positive"));
        status->setAccessibleName(tr("Machine %1 is available").arg(id));
        nameLine->addWidget(status);
        auto* via = new QLabel(tr("Relay"), row);
        via->setProperty("remoteRole", QStringLiteral("badge"));
        nameLine->addWidget(via);
        nameLine->addStretch(1);
        copy->addLayout(nameLine);
        auto* meta = new QLabel(tr("Linux · Trusted"), row);
        meta->setProperty("remoteRole", QStringLiteral("deviceMeta"));
        copy->addWidget(meta);
        copy->addStretch(1);
        layout->addLayout(copy, 1);
        rows_->addWidget(row);
    }
    if (pairingEntryShown_) {
        auto* footer = new QWidget(this);
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(0, 0, 0, 0);
        addConnectButton(layout);
        rows_->addWidget(footer);
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

LinuxMachinesSection::~LinuxMachinesSection() { delete pairingDialog_.data(); }

void LinuxMachinesSection::startPairing() {
    if (pairingDialog_)
        return;
    // An in-window card like the other dialogs (#422); it goes once it finishes.
    auto* dialog = new ket::Dialog(tr("Pair a Linux machine"), this);
    dialog->setObjectName(QStringLiteral("linuxPairingDialog"));
    pairingDialog_ = dialog;
    dialog->setCardWidth(470);
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(QMargins());
    auto* instructions = new QLabel(tr("Run ketplusctl pairing create on the Linux machine, "
                                       "then paste the full ketplus:// pairing link."),
                                    body);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    auto* input = new ket::TextField(body);
    input->setObjectName(QStringLiteral("linuxPairingLink"));
    input->setAccessibleName(tr("One-time Linux pairing link"));
    input->setEchoMode(QLineEdit::Password);
    input->setPlaceholderText(QStringLiteral("ketplus://pair?ticket=…&secret=…"));
    layout->addWidget(input);
    auto* feedback = new QLabel(body);
    feedback->setObjectName(QStringLiteral("linuxPairingFeedback"));
    feedback->setWordWrap(true);
    layout->addWidget(feedback);
    auto* match = new QCheckBox(tr("I checked the identity on both machines."), body);
    match->hide();
    layout->addWidget(match);
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    auto* check = dialog->addButton(tr("Check link"));
    auto* approve = dialog->addButton(tr("Check approval"), ket::ButtonVariant::Primary);
    approve->setEnabled(false);
    auto ticketId = std::make_shared<QString>();
    const QPointer<ket::Dialog> dialogGuard(dialog);
    connect(dialog, &QObject::destroyed, this, [this] { client_->cancelPairing(); });
    connect(dialog, &ket::Dialog::rejected, this, [this] { client_->cancelPairing(); });
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
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
    dialog->open();
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
