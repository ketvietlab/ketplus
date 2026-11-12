#include "remote/LinuxMachinesSection.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "KetTokens.h"

#include "KetPlusProductVersion.h"
#include "control/ControlV2Client.h"
#include "remote/LinuxConnectDialog.h"
#include "remote/LinuxMachineRow.h"

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
namespace {} // namespace

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
        new QLabel(tr("Linux machines connect over your SSH access (free) or through the "
                      "ketPlus Relay (Pro). Verify the host fingerprint or pairing code before "
                      "trusting one."),
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
    setModel(new LinuxMachineModel(this));
}

QStringList LinuxMachinesSection::machineIds() const {
    QStringList ids;
    for (const auto& machine : model_->machines())
        ids.append(machine.id);
    return ids;
}

void LinuxMachinesSection::setModel(LinuxMachineModel* model) {
    if (!model || model == model_)
        return;
    if (model_) {
        disconnect(model_, nullptr, this, nullptr);
        if (model_->parent() == this)
            model_->deleteLater();
    }
    model_ = model;
    connect(model_, &LinuxMachineModel::machinesChanged, this, [this] {
        render();
        QStringList ids;
        QStringList names;
        for (const auto& machine : model_->machines()) {
            ids.append(machine.id);
            names.append(machine.name);
        }
        emit machinesChanged(ids, names);
    });
    connect(model_, &QObject::destroyed, this, [this] {
        model_ = nullptr;
        setModel(new LinuxMachineModel(this));
    });
    render();
}

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
    const auto& machines = model_->machines();
    count_->setText(tr("%1 trusted").arg(machines.size()));
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
    const auto addRows = [this, &machines] {
        // A device row as the phones above: icon, name, its state and route, and its actions.
        for (const auto& machine : machines)
            rows_->addWidget(makeLinuxMachineRow(machine, model_, this));
    };
    if (!client_->signedIn()) {
        // SSH machines need no account; the Relay does.
        addRows();
        auto* layout = message(tr("Sign in with the same KetSuite account used by the headless "
                                  "Linux daemon."));
        auto* signInButton = new ket::Button(tr("Sign in to connect a Linux machine"),
                                             ket::ButtonVariant::Primary, this);
        connect(signInButton, &QAbstractButton::clicked, this, &LinuxMachinesSection::signIn);
        layout->addWidget(signInButton, 0, Qt::AlignLeft);
        return;
    }
    if (loading_ && machines.isEmpty()) {
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
    if (machines.isEmpty()) {
        if (pairingEntryShown_) {
            addConnectButton(message(tr("No Linux machines yet. Start pairing on the Linux "
                                        "daemon, then paste its one-time pairing link here.")));
        } else {
            message(tr("No Linux machines yet. Start pairing on the Linux daemon, then choose "
                       "Add › Connect a Linux machine and paste its one-time pairing link."));
        }
        return;
    }
    addRows();
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

LinuxMachinesSection::~LinuxMachinesSection() {
    if (model_)
        disconnect(model_, nullptr, this, nullptr);
    delete connectDialog_.data();
}

void LinuxMachinesSection::beginPairing() {
    if (connectDialog_)
        return;
    // SSH needs no account, so the card opens signed out too; its Relay mode offers sign-in.
    connectDialog_ = openLinuxConnectDialog(this, model_, client_,
                                            {[this] { signIn(); },
                                             [this](const QString& message) {
                                                 setNotice(message);
                                                 refreshMachines();
                                             }});
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
        model_->setRelayMachines(ids);
        render();
    });
}

} // namespace ketplus
