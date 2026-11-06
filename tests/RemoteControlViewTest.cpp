#include "remote/RemoteControlView.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include <algorithm>

namespace ketplus {

class RemoteControlViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void opensOnSignInPrompt();
    void signInFormIsAnExplicitSecondaryFlow();
    void demoSignInOpensAddDeviceQr();
    void demoSignOutReturnsToSingleAction();
    void aClaimThatChangesAfterItsCodeWasShownCannotBeApproved();
    void pairingShowsOnlyTheDaemonsCodeAndForgetsAFinishedTicket();
};

void RemoteControlViewTest::aClaimThatChangesAfterItsCodeWasShownCannotBeApproved() {
    RemoteControlView view;
    const auto claim = [](const QString& digest) {
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("claimed")},
                           {QStringLiteral("claimDigest"), digest},
                           {QStringLiteral("claimedDevice"),
                            QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Phone")}}}};
    };
    const auto approvable = [&view] {
        return view.approveButton_->property("pairAction").toString() == QStringLiteral("approve");
    };
    view.ticketId_ = QStringLiteral("ticket");
    view.showPairing(claim(QStringLiteral("digest-a")), QStringLiteral("ABC123"));
    QVERIFY(approvable());
    QCOMPARE(view.shownClaimDigest_, QStringLiteral("digest-a"));
    // The relay clears the digest, then presents another claim: neither replaces the one shown.
    view.showPairing(claim({}), QStringLiteral("ABC123"));
    QVERIFY(!approvable());
    view.showPairing(claim(QStringLiteral("digest-b")), QStringLiteral("ABC123"));
    QVERIFY(!approvable());
    view.showPairing(claim(QStringLiteral("digest-a")), QStringLiteral("ABC123"));
    QVERIFY(!approvable());
    QCOMPARE(view.shownClaimDigest_, QStringLiteral("digest-a"));
    // Closing the dialog starts the next ticket clean (no ticket, so nothing is revoked).
    view.ticketId_.clear();
    view.closePairing();
    view.ticketId_ = QStringLiteral("next");
    view.showPairing(claim(QStringLiteral("digest-b")), QStringLiteral("ABC123"));
    QVERIFY(approvable());
}

void RemoteControlViewTest::pairingShowsOnlyTheDaemonsCodeAndForgetsAFinishedTicket() {
    RemoteControlView view;
    const QJsonObject claimed{
        {QStringLiteral("status"), QStringLiteral("claimed")},
        {QStringLiteral("claimDigest"), QStringLiteral("digest")},
        {QStringLiteral("claimedDevice"),
         QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Phone")}}}};
    const auto approvable = [&view] {
        return view.approveButton_->property("pairAction").toString() == QStringLiteral("approve");
    };
    view.ticketId_ = QStringLiteral("ticket");
    view.pairingCode_->setText(QStringLiteral("ketplus://pair?ticket=t&secret=s"));
    view.showPairing({{QStringLiteral("status"), QStringLiteral("open")},
                      {QStringLiteral("expiresAt"),
                       QDateTime::currentDateTimeUtc().addSecs(125).toString(Qt::ISODateWithMs)}});
    QVERIFY(view.pairingTimer_->text().startsWith(QStringLiteral("Code expires in 2:0")));

    // Without a code from the daemon (an unverified claim), there is nothing to compare: no
    // fixed fallback code, and no approval.
    view.showPairing(claimed);
    QVERIFY(!approvable());
    QVERIFY(view.pairingConfirmationCode_->text() != QStringLiteral("482 619"));
    QVERIFY(view.pairingCode_->text().isEmpty());

    view.ticketId_.clear(); // nothing to revoke
    view.closePairing();
    view.ticketId_ = QStringLiteral("next");
    view.showPairing(claimed, QStringLiteral("ABC123"));
    QVERIFY(approvable());
    QCOMPARE(view.pairingConfirmationCode_->text(), QStringLiteral("ABC 123"));
    QVERIFY(!view.deviceClaim_->text().contains(QStringLiteral("Account")));

    view.pairingCode_->setText(QStringLiteral("ketplus://pair?ticket=t&secret=s"));
    // The relay keeps the claim's digest on a finished ticket.
    view.showPairing({{QStringLiteral("status"), QStringLiteral("revoked")},
                      {QStringLiteral("claimDigest"), QStringLiteral("digest")}});
    QCOMPARE(view.pairingDialogTitle_->text(), QStringLiteral("Pairing cancelled"));
    QVERIFY(view.pairingCode_->text().isEmpty());
    // The relay keeps the claim's digest on a finished ticket.
    view.showPairing({{QStringLiteral("status"), QStringLiteral("expired")},
                      {QStringLiteral("claimDigest"), QStringLiteral("digest")}});
    QCOMPARE(view.pairingDialogTitle_->text(), QStringLiteral("Pairing code expired"));

    view.pairingCode_->setText(QStringLiteral("ketplus://pair?ticket=t&secret=s"));
    view.ticketId_.clear(); // nothing to revoke
    view.closePairing();
    QVERIFY(view.pairingCode_->text().isEmpty());
}

void RemoteControlViewTest::opensOnSignInPrompt() {
    RemoteControlView view;
    view.resize(840, 720);
    view.show();
    QTest::qWait(1);

    auto* signInPrompt = view.findChild<QPushButton*>(QStringLiteral("remoteSignInPromptButton"));
    QVERIFY(signInPrompt);
    QVERIFY(signInPrompt->isVisible());
    const auto buttons = view.findChildren<QPushButton*>();
    QVERIFY(std::count_if(buttons.cbegin(), buttons.cend(),
                          [](const QPushButton* button) { return button->isVisible(); }) >= 2);
    QVERIFY(view.findChild<QWidget*>(QStringLiteral("linuxMachinesSection")));
    auto* pair = view.findChild<QPushButton*>(QStringLiteral("remotePairButton"));
    QVERIFY(pair);
    QVERIFY(!pair->isVisible());
    QVERIFY(!pair->isEnabled());
    auto* title = view.findChild<QLabel*>(QStringLiteral("remoteWelcomeTitle"));
    auto* text = view.findChild<QLabel*>(QStringLiteral("remoteWelcomeText"));
    QVERIFY(title && text);
    QCOMPARE(title->text(), QStringLiteral("Connections"));
    QCOMPARE(text->text(), QStringLiteral("Sign in to connect your phone to this Mac."));
    QVERIFY(title->font().pixelSize() >= 28);
    QVERIFY(!view.findChild<QWidget*>(QStringLiteral("remotePageTitle"))->isVisible());
}

void RemoteControlViewTest::signInFormIsAnExplicitSecondaryFlow() {
    RemoteControlView view;
    view.resize(1100, 720);
    view.show();
    QTest::qWait(1);

    auto* signInPrompt = view.findChild<QPushButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QPushButton*>(QStringLiteral("remoteSignInButton"));
    auto* back = view.findChild<QPushButton*>(QStringLiteral("remoteSignInBackButton"));
    auto* dialog = view.findChild<QDialog*>(QStringLiteral("remoteSignInDialog"));
    QVERIFY(signInPrompt);
    QVERIFY(signIn);
    QVERIFY(back);
    QVERIFY(dialog);
    QVERIFY(!signIn->isVisible());

    QTest::mouseClick(signInPrompt, Qt::LeftButton);
    QVERIFY(signIn->isVisible());
    QCOMPARE(dialog->focusWidget(), signIn);
    QCOMPARE(dialog->width(), 500);
    QCOMPARE(signIn->text(), QStringLiteral("Continue in browser"));
    // The password is typed only on the tenant's own sign-in page, never into KetPlus.
    QVERIFY(dialog->findChildren<QLineEdit*>().isEmpty());
    QTest::mouseClick(back, Qt::LeftButton);
    QVERIFY(signInPrompt->isVisible());
}

void RemoteControlViewTest::demoSignInOpensAddDeviceQr() {
#if !defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
    QSKIP("Synthetic remote data is compiled only into opted-in Debug builds.");
#else
    qputenv("KETPLUS_REMOTE_DEMO", "1");
    RemoteControlView view;
    view.resize(840, 720);
    view.show();
    QTest::qWait(1);

    auto* prompt = view.findChild<QPushButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QPushButton*>(QStringLiteral("remoteSignInButton"));
    auto* add = view.findChild<QPushButton*>(QStringLiteral("remotePairButton"));
    auto* dialog = view.findChild<QDialog*>(QStringLiteral("remotePairingDialog"));
    auto* qr = view.findChild<QLabel*>(QStringLiteral("remotePairingQr"));
    auto* close = view.findChild<QPushButton*>(QStringLiteral("remotePairingCloseButton"));
    auto* pairTitle = view.findChild<QLabel*>(QStringLiteral("remotePairingState"));
    auto* pairDialogTitle = view.findChild<QLabel*>(QStringLiteral("remotePairingDialogTitle"));
    auto* pairHelp = view.findChild<QLabel*>(QStringLiteral("remotePairingHelp"));
    auto* pairTimer = view.findChild<QLabel*>(QStringLiteral("remotePairingTimer"));
    auto* pairSteps = view.findChild<QLabel*>(QStringLiteral("remotePairingSteps"));
    auto* pairCode = view.findChild<QLabel*>(QStringLiteral("remotePairingConfirmationCode"));
    auto* pairAction = view.findChild<QPushButton*>(QStringLiteral("remoteApproveButton"));
    auto* pairCancel = view.findChild<QPushButton*>(QStringLiteral("remotePairingCancelButton"));
    QVERIFY(prompt && signIn && add && dialog && qr && close && pairTitle && pairDialogTitle &&
            pairHelp && pairTimer && pairSteps && pairCode && pairAction && pairCancel);
    QCOMPARE(add->text(), QStringLiteral("Add"));
    QCOMPARE(qr->size(), QSize(204, 204));

    QTest::mouseClick(prompt, Qt::LeftButton);
    QTest::mouseClick(signIn, Qt::LeftButton);
    QVERIFY(add->isVisible());
    QVERIFY(add->isEnabled());
    auto* pageTitle = view.findChild<QLabel*>(QStringLiteral("remotePageTitle"));
    auto* pageSubtitle = view.findChild<QLabel*>(QStringLiteral("remotePageSubtitle"));
    auto* overviewDescription =
        view.findChild<QLabel*>(QStringLiteral("remoteOverviewDescription"));
    QVERIFY(pageTitle && pageSubtitle && overviewDescription);
    QVERIFY(pageTitle->isVisible());
    QCOMPARE(
        pageSubtitle->text(),
        QStringLiteral("Phones that access this Mac and Linux machines this desktop can use."));
    QCOMPARE(overviewDescription->text(), QStringLiteral("Ready for your connected devices"));
    auto* overview = view.findChild<QWidget*>(QStringLiteral("remoteConnectionOverview"));
    QVERIFY(overview && overview->isVisible());
    auto* emptyDevices = view.findChild<QLabel*>(QStringLiteral("remoteDevicesEmpty"));
    QVERIFY(emptyDevices && emptyDevices->isVisible());
    QCOMPARE(emptyDevices->text(), QStringLiteral("No devices connected yet."));
    auto* devices = view.findChild<QWidget*>(QStringLiteral("remoteDevicesPanel"));
    QVERIFY(devices);
    QVERIFY(devices->width() >= 650);
    QVERIFY(devices->width() < view.width());

    QTimer::singleShot(0, &view, [] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        menu->actions().first()->trigger();
    });
    QTest::mouseClick(add, Qt::LeftButton);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->width(), 500);
    QVERIFY(!qr->pixmap().isNull());
    QCOMPARE(pairTitle->text(), QStringLiteral("Take your workspace with you."));
    QCOMPARE(pairHelp->text(), QStringLiteral("In KetPlus on your phone, choose Scan QR code."));
    QVERIFY(pairTimer->isVisible());
    QVERIFY(pairSteps->isVisible());
    QCOMPARE(pairAction->text(), QStringLiteral("Simulate scan"));
    QTest::mouseClick(pairAction, Qt::LeftButton);
    QCOMPARE(pairTitle->text(), QStringLiteral("Is this your phone?"));
    QVERIFY(!qr->isVisible());
    QVERIFY(pairCode->isVisible());
    QCOMPARE(pairCode->text(), QStringLiteral("482 619"));
    QCOMPARE(pairAction->text(), QStringLiteral("Yes, connect this device"));
    QTest::mouseClick(pairAction, Qt::LeftButton);
    QTRY_VERIFY(!dialog->isVisible());
    QTest::mouseClick(close, Qt::LeftButton);
    QVERIFY(!dialog->isVisible());
    QTimer::singleShot(0, &view, [] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        menu->actions().first()->trigger();
    });
    QTest::mouseClick(add, Qt::LeftButton);
    QVERIFY(dialog->isVisible());
    QCOMPARE(pairCancel->text(), QStringLiteral("Expire code"));
    QTest::mouseClick(pairCancel, Qt::LeftButton);
    QCOMPARE(pairDialogTitle->text(), QStringLiteral("Pairing code expired"));
    QCOMPARE(pairAction->text(), QStringLiteral("Generate new code"));
    QTest::keyClick(dialog, Qt::Key_Escape);
    QVERIFY(!dialog->isVisible());
    qunsetenv("KETPLUS_REMOTE_DEMO");
#endif
}

void RemoteControlViewTest::demoSignOutReturnsToSingleAction() {
#if !defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
    QSKIP("Synthetic remote data is compiled only into opted-in Debug builds.");
#else
    qputenv("KETPLUS_REMOTE_DEMO", "1");
    RemoteControlView view;
    view.resize(840, 720);
    view.show();
    auto* prompt = view.findChild<QPushButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QPushButton*>(QStringLiteral("remoteSignInButton"));
    auto* signOut = view.findChild<QPushButton*>(QStringLiteral("remoteSignOutButton"));
    QVERIFY(prompt && signIn && signOut);
    QTest::mouseClick(prompt, Qt::LeftButton);
    QTest::mouseClick(signIn, Qt::LeftButton);
    QVERIFY(signOut->isVisible());
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->accept();
    });
    QTest::mouseClick(signOut, Qt::LeftButton);
    QVERIFY(prompt->isVisible());
    QVERIFY(!signOut->isVisible());
    qunsetenv("KETPLUS_REMOTE_DEMO");
#endif
}

} // namespace ketplus

QTEST_MAIN(ketplus::RemoteControlViewTest)
#include "RemoteControlViewTest.moc"
