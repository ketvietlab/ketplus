#include "remote/LinuxDaemonInstallCard.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetTokens.h"
#include "remote/LinuxMachineModel.h"
#include <QCoreApplication>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {
QString tr(const char* text) { return QCoreApplication::translate("LinuxDaemonInstallCard", text); }
} // namespace
QWidget* makeLinuxDaemonInstallCard(LinuxMachineModel* model, std::shared_ptr<QString> pendingHost,
                                    QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName("linuxDaemonInstallCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins({});
    layout->setSpacing(12);
    auto* copy = new QLabel(card);
    copy->setObjectName("linuxDaemonInstallDescription");
    copy->setWordWrap(true);
    copy->setTextFormat(Qt::PlainText);
    copy->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const auto applyText = [copy] {
        auto palette = copy->palette();
        palette.setColor(QPalette::WindowText,
                         ket::Tokens::color(u"text-main", ket::Appearance::instance().theme()));
        copy->setPalette(palette);
    };
    QObject::connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, copy, applyText);
    applyText();
    layout->addWidget(copy);
    auto* review =
        new ket::Button(tr("Review signed install / upgrade"), ket::ButtonVariant::Ghost, card);
    review->setObjectName("linuxDaemonReviewInstall");
    auto* start = new ket::Button(tr("Start managed daemon"), ket::ButtonVariant::Ghost, card);
    start->setObjectName("linuxDaemonStart");
    auto* uninstall =
        new ket::Button(tr("Uninstall managed daemon…"), ket::ButtonVariant::Ghost, card);
    uninstall->setObjectName("linuxDaemonUninstall");
    auto* confirm =
        new ket::Button(tr("Confirm daemon changes"), ket::ButtonVariant::Primary, card);
    confirm->setObjectName("linuxDaemonConfirm");
    auto* decline = new ket::Button(tr("Decline"), ket::ButtonVariant::Ghost, card);
    decline->setObjectName("linuxDaemonDecline");
    for (auto* button : {review, start, uninstall, confirm, decline})
        layout->addWidget(button);
    confirm->hide();
    decline->hide();
    card->hide();
    const QPointer<LinuxMachineModel> guard(model);
    const auto begin = [guard, pendingHost, review, start, uninstall,
                        confirm](const QString& action) {
        confirm->setText(action == "install" ? tr("Install / upgrade daemon")
                         : action == "start" ? tr("Start daemon")
                                             : tr("Uninstall daemon"));
        review->setEnabled(false);
        start->setEnabled(false);
        uninstall->setEnabled(false);
        review->hide();
        start->hide();
        uninstall->hide();
        if (guard)
            guard->requestDaemonReview(*pendingHost, action);
    };
    QObject::connect(review, &QAbstractButton::clicked, card, [begin] { begin("install"); });
    QObject::connect(start, &QAbstractButton::clicked, card, [begin] { begin("start"); });
    QObject::connect(uninstall, &QAbstractButton::clicked, card, [begin] { begin("uninstall"); });
    QObject::connect(confirm, &QAbstractButton::clicked, card,
                     [guard, pendingHost, confirm, decline] {
                         confirm->setEnabled(false);
                         decline->hide();
                         if (guard)
                             guard->decideDaemonInstall(*pendingHost, true);
                     });
    QObject::connect(decline, &QAbstractButton::clicked, card, [guard, pendingHost] {
        if (guard)
            guard->decideDaemonInstall(*pendingHost, false);
    });
    if (!model)
        return card;
    QObject::connect(
        model, &LinuxMachineModel::daemonInstallOffered, card,
        [pendingHost, card, copy, review, start, uninstall, confirm, decline](const QString& host,
                                                                              bool configured) {
            if (host != *pendingHost)
                return;
            copy->setText(configured
                              ? tr("No daemon connection yet. Review the signed release metadata "
                                   "before installing. No remote files change until you confirm.")
                              : tr("Install unavailable: no trusted release channel. Nothing will "
                                   "be downloaded. An existing ketPlus-managed installation can "
                                   "still be started or uninstalled."));
            review->setEnabled(configured);
            start->setEnabled(true);
            uninstall->setEnabled(true);
            review->show();
            start->show();
            uninstall->show();
            confirm->hide();
            decline->hide();
            card->show();
        });
    QObject::connect(
        model, &LinuxMachineModel::daemonConsentRequested, card,
        [pendingHost, copy, confirm, decline](const QString& host, const QString& description) {
            if (host != *pendingHost)
                return;
            copy->setText(description);
            confirm->setEnabled(true);
            confirm->show();
            decline->show();
            decline->setFocus(); // Enter never implicitly consents to an installation.
        });
    QObject::connect(model, &LinuxMachineModel::daemonInstallProgress, card,
                     [pendingHost, copy](const QString& host, const QString& message) {
                         if (host == *pendingHost)
                             copy->setText(message);
                     });
    QObject::connect(model, &LinuxMachineModel::sshFailed, card,
                     [card, pendingHost](const QString& host, LinuxMachine::Error, const QString&) {
                         if (host == *pendingHost)
                             card->hide();
                     });
    return card;
}
} // namespace ketplus
