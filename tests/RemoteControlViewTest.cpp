#include "remote/RemoteControlView.h"
#include "remote/RemoteActivitySection.h"
#include "remote/RemoteConnectionsIntro.h"
#include "ui/InterfaceZoom.h"

#include <QAccessible>
#include <QJsonArray>
#include <QTimeZone>

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QTest>
#include <QTimer>

#include <algorithm>

namespace ketplus {

class RemoteControlViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void opensOnSignInPrompt();
    void signedOutIntroFollowsTheMockAtEveryWidth();
    void signInFormIsAnExplicitSecondaryFlow();
    void demoSignInOpensAddDeviceQr();
    void demoSignOutReturnsToSingleAction();
    void aClaimThatChangesAfterItsCodeWasShownCannotBeApproved();
    void pairingShowsOnlyTheDaemonsCodeAndForgetsAFinishedTicket();
    void activityWordsFollowTheAudit();
    void activityRowsAreReadableFromTheKeyboardAndScreenReaders();
    void activityDropsRepliesFromBeforeSignOut();
};

namespace {
QJsonObject auditEntry(const QString& id, const QString& kind, const QString& device,
                       const QString& result, const QDateTime& at, const QString& session = {},
                       const bool exists = true, const QString& title = {}) {
    return {{QStringLiteral("command_id"), id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("device_id"), device},
            {QStringLiteral("device_key_version"), 1},
            {QStringLiteral("session_id"), session},
            {QStringLiteral("session_exists"), exists},
            {QStringLiteral("session_title"), title},
            {QStringLiteral("result_code"), result},
            {QStringLiteral("recorded_at"), at.toUTC().toString(Qt::ISODateWithMs)}};
}
} // namespace

void RemoteControlViewTest::activityWordsFollowTheAudit() {
    using namespace remote_activity;
    QCOMPARE(action(QStringLiteral("session.start_turn")), QStringLiteral("Sent a message"));
    QCOMPARE(action(QStringLiteral("session.create")), QStringLiteral("Started a conversation"));
    // A kind this build does not know is still named.
    QCOMPARE(action(QStringLiteral("future.kind")), QStringLiteral("Sent a future.kind request"));

    QCOMPARE(outcome(QStringLiteral("ok")).text, QStringLiteral("Done"));
    QVERIFY(outcome(QStringLiteral("ok")).reason.isEmpty());
    QCOMPARE(outcome(QStringLiteral("worktree_quota_exceeded")).reason,
             QStringLiteral("Worktree limit reached"));
    QCOMPARE(outcome(QStringLiteral("worktree_quota_exceeded")).tone, QStringLiteral("warning"));
    // A sender that did not verify is only the device it claimed to be.
    QCOMPARE(outcome(QStringLiteral("invalid_encrypted_command")).tone, QStringLiteral("danger"));
    QCOMPARE(outcome(QStringLiteral("sender_key_unavailable")).reason,
             QStringLiteral("Sender not verified"));
    QCOMPARE(outcome(QStringLiteral("branch_exists")).reason, QStringLiteral("branch_exists"));

    const QDateTime now(QDate(2026, 11, 6), QTime(15, 30), QTimeZone::LocalTime);
    QCOMPARE(when(now.addSecs(-20), now), QStringLiteral("Just now"));
    QCOMPARE(when(now.addSecs(-12 * 60), now), QStringLiteral("12 min ago"));
    QCOMPARE(when(now.addSecs(-3 * 3600), now), QStringLiteral("3 hours ago"));
    QCOMPARE(when(QDateTime(QDate(2026, 11, 5), QTime(21, 14), QTimeZone::LocalTime), now),
             QStringLiteral("Yesterday, 21:14"));
    QCOMPARE(when(QDateTime(QDate(2026, 11, 2), QTime(9, 2), QTimeZone::LocalTime), now),
             QStringLiteral("Mon, 09:02"));
    QCOMPARE(when(QDateTime(QDate(2026, 10, 28), QTime(17, 55), QTimeZone::LocalTime), now),
             QStringLiteral("28 Oct, 17:55"));

    QCOMPARE(target(auditEntry("a", "session.start_turn", "d", "ok", now, "s", true, "Fix login")),
             QStringLiteral("Fix login"));
    QCOMPARE(target(auditEntry("a", "session.delete", "d", "ok", now, "s", false)),
             QStringLiteral("Deleted conversation"));
    QCOMPARE(target(auditEntry("a", "session.rename", "d", "ok", now, "s", true, {})),
             QStringLiteral("Untitled conversation"));
    // A refused new conversation names none.
    QVERIFY(
        target(auditEntry("a", "session.create", "d", "worktree_quota_exceeded", now)).isEmpty());
    const QHash<QString, QString> names{{QStringLiteral("phone-1"), QStringLiteral("iPhone 16")}};
    QCOMPARE(device(auditEntry("a", "k", "phone-1", "ok", now), names),
             QStringLiteral("iPhone 16"));
    QCOMPARE(device(auditEntry("a", "k", "phone-9", "ok", now), names),
             QStringLiteral("Unknown device"));
}

void RemoteControlViewTest::activityRowsAreReadableFromTheKeyboardAndScreenReaders() {
    RemoteActivitySection section(nullptr);
    section.setDeviceNames({{QStringLiteral("phone-1"), QStringLiteral("iPhone 16")}});
    section.resize(900, 600);
    section.show();
    const auto rows = [&section] {
        QList<QFrame*> found;
        for (QFrame* frame : section.findChildren<QFrame*>()) {
            if (frame->property("remoteRole").toString() == QStringLiteral("activityRow"))
                found.append(frame);
        }
        return found;
    };
    auto* showAll = section.findChild<QAbstractButton*>(QStringLiteral("remoteActivityShowAll"));
    QVERIFY(showAll);

    section.render({});
    QTest::qWait(1);
    QVERIFY(section.findChild<QLabel*>(QStringLiteral("remoteActivityEmpty")));
    QVERIFY(!showAll->isVisible());

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonArray entries;
    entries.append(auditEntry("c0", "session.create", "phone-1", "worktree_quota_exceeded",
                              now.addSecs(-120)));
    for (int index = 1; index < 7; ++index)
        entries.append(auditEntry(QStringLiteral("c%1").arg(index), "session.start_turn", "phone-1",
                                  "ok", now.addSecs(-120 - index * 600), "session-1", true,
                                  QStringLiteral("A conversation title long enough to be cut "
                                                 "short in a narrow window %1")
                                      .arg(index)));
    section.render(entries);
    QTest::qWait(1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QList<QFrame*> shown = rows();
    // The newest five; the sixth entry asked for says there are more.
    QCOMPARE(shown.size(), RemoteActivitySection::shownRows);
    QVERIFY(showAll->isVisible());

    QFrame* first = shown.first();
    QCOMPARE(first->accessibleName(),
             QStringLiteral("Started a conversation, 2 min ago. Refused: Worktree limit reached. "
                            "iPhone 16."));
    QVERIFY(first->focusPolicy() & Qt::TabFocus);
    QAccessibleInterface* item = QAccessible::queryAccessibleInterface(first);
    QVERIFY(item);
    QCOMPARE(item->role(), QAccessible::ListItem);
    QAccessibleInterface* list =
        QAccessible::queryAccessibleInterface(section.findChild<QFrame*>("remoteActivityPanel"));
    QVERIFY(list);
    QCOMPARE(list->role(), QAccessible::List);

    // The arrow keys move between rows.
    first->setFocus(Qt::TabFocusReason);
    QTest::keyClick(first, Qt::Key_Down);
    QCOMPARE(QApplication::focusWidget(), shown.at(1));

    // Cut short, a row offers its whole entry; whole, it needs no tooltip.
    QFrame* second = shown.at(1);
    QVERIFY(second->toolTip().isEmpty());
    section.resize(360, 600);
    QTest::qWait(1);
    QApplication::processEvents();
    QCOMPARE(second->toolTip(), second->accessibleName());
    QVERIFY(second->accessibleName().contains(QStringLiteral("narrow window 1.")));

    // A refresh rebuilds the rows but keeps the one being read focused...
    const auto focusedCommand = [] {
        QWidget* focused = QApplication::focusWidget();
        return focused ? focused->property("remoteCommandId").toString() : QString();
    };
    second->setFocus(Qt::TabFocusReason);
    section.render(entries);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(focusedCommand(), QStringLiteral("c1"));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
    QCOMPARE(focusedCommand(), QStringLiteral("c2"));
    // ...or, when its entry is gone, the row now in its place.
    QJsonArray without = entries;
    without.removeAt(2);
    section.render(without);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(focusedCommand(), QStringLiteral("c3"));
    // A new request arriving above it does not move focus off the entry.
    QJsonArray newer{auditEntry("c-new", "session.interrupt", "phone-1", "ok", now)};
    for (const QJsonValue& entry : std::as_const(without))
        newer.append(entry);
    section.render(newer);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(focusedCommand(), QStringLiteral("c3"));
}

void RemoteControlViewTest::activityDropsRepliesFromBeforeSignOut() {
    RemoteActivitySection section(nullptr);
    section.setDeviceNames({{QStringLiteral("phone-1"), QStringLiteral("iPhone 16")}});
    const auto shownRows = [&section] {
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        int count = 0;
        for (QFrame* frame : section.findChildren<QFrame*>())
            count += frame->property("remoteRole").toString() == QStringLiteral("activityRow");
        return count;
    };
    const QJsonObject reply{
        {QStringLiteral("entries"), QJsonArray{auditEntry("c0", "session.start_turn", "phone-1",
                                                          "ok", QDateTime::currentDateTimeUtc())}}};

    // A request is outstanding when the account signs out.
    section.requestInFlight_ = true;
    const quint64 beforeSignOut = section.generation_;
    section.clear();
    QVERIFY(!section.requestInFlight_);
    QVERIFY(section.names_.isEmpty());
    section.received(beforeSignOut, reply, {});
    QCOMPARE(shownRows(), 0);
    QVERIFY(section.entries_.isEmpty());

    // The next account's own reply is shown.
    section.received(section.generation_, reply, {});
    QCOMPARE(shownRows(), 1);
}

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

    auto* signInPrompt =
        view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInPromptButton"));
    QVERIFY(signInPrompt);
    QVERIFY(signInPrompt->isVisible());
    const auto buttons = view.findChildren<QAbstractButton*>();
    QVERIFY(std::count_if(buttons.cbegin(), buttons.cend(),
                          [](const QAbstractButton* button) { return button->isVisible(); }) >= 2);
    QVERIFY(view.findChild<QWidget*>(QStringLiteral("linuxMachinesSection")));
    auto* pair = view.findChild<QAbstractButton*>(QStringLiteral("remotePairButton"));
    QVERIFY(pair);
    QVERIFY(!pair->isVisible());
    QVERIFY(!pair->isEnabled());
    auto* title = view.findChild<QLabel*>(QStringLiteral("remoteWelcomeTitle"));
    auto* text = view.findChild<QLabel*>(QStringLiteral("remoteWelcomeText"));
    QVERIFY(title && text);
    QCOMPARE(title->text(), QStringLiteral("Connections"));
    QVERIFY(text->text().startsWith(QStringLiteral("Continue a conversation from your phone")));
    auto* headline = view.findChild<QLabel*>(QStringLiteral("remoteWelcomeHeadline"));
    QVERIFY(headline);
    const int basePixels = qApp->font().pixelSize() > 0 ? qApp->font().pixelSize()
                                                        : InterfaceZoomMetrics::mediumPixels;
    QCOMPARE(headline->font().pixelSize(), InterfaceZoomMetrics::geometry(40, basePixels));
    QCOMPARE(signInPrompt->text(), QStringLiteral("Sign in"));
    QVERIFY(!view.findChild<QWidget*>(QStringLiteral("remotePageTitle"))->isVisible());
}

void RemoteControlViewTest::signedOutIntroFollowsTheMockAtEveryWidth() {
    for (const auto* name : {"desktop-question", "mobile-question"})
        for (const auto* theme : {"light", "dark"})
            QVERIFY2(!QPixmap(QStringLiteral(":/connections/%1-%2.png")
                                  .arg(QLatin1String(name), QLatin1String(theme)))
                          .isNull(),
                     name);

    RemoteControlView view;
    view.resize(1100, 900);
    view.show();
    QTest::qWait(1);
    RemoteConnectionsIntro* intro = view.intro_;
    QVERIFY(intro && intro->isVisible());
    QVERIFY(!intro->narrow_);
    auto* scene = intro->findChild<QWidget*>(QStringLiteral("remoteConnectionsScene"));
    QVERIFY(scene);
    QCOMPARE(scene->height(), 400);
    QVERIFY(scene->x() > intro->copy_->x());
    const auto position = [](QGridLayout* grid, QWidget* widget) {
        int row = 0;
        int column = 0;
        int rowSpan = 0;
        int columnSpan = 0;
        grid->getItemPosition(grid->indexOf(widget), &row, &column, &rowSpan, &columnSpan);
        return QPoint(column, row);
    };
    for (int index = 0; index < 3; ++index)
        QCOMPARE(position(intro->steps_, intro->stepCells_[index]), QPoint(index, 0));
    QCOMPARE(position(intro->stepsHeader_, intro->stepsNote_), QPoint(1, 0));
    QCOMPARE(intro->stepCells_[1]->accessibleName(),
             QStringLiteral("Step 2: Scan with KetPlus Mobile. Open the app on your phone and "
                            "scan the pairing code."));

    // Below 860 px the scene goes under the copy and the steps stack.
    view.resize(760, 900);
    QTest::qWait(1);
    QVERIFY(intro->narrow_);
    QCOMPARE(scene->height(), 380);
    QVERIFY(scene->width() <= 500);
    QVERIFY(scene->width() >= 400);
    QVERIFY(scene->y() > intro->copy_->y() + intro->copy_->height() - 1);
    for (int index = 0; index < 3; ++index)
        QCOMPARE(position(intro->steps_, intro->stepCells_[index]), QPoint(0, index));
    QCOMPARE(position(intro->stepsHeader_, intro->stepsNote_), QPoint(0, 1));
    view.resize(1100, 900);
    QTest::qWait(1);
    QVERIFY(!intro->narrow_);
    QCOMPARE(position(intro->steps_, intro->stepCells_[2]), QPoint(2, 0));

    // An expired login says so above the introduction; signing in again hides it.
    auto* expired = view.findChild<QWidget*>(QStringLiteral("remoteExpiredBanner"));
    QVERIFY(expired && !expired->isVisible());
    view.applyStatus({{QStringLiteral("configured"), false},
                      {QStringLiteral("last_error"), QStringLiteral("token expired")}});
    QVERIFY(expired->isVisible());
    QVERIFY(!view.findChild<QWidget*>(QStringLiteral("remoteWelcomeNotice"))->isVisible());
    view.applyStatus({{QStringLiteral("configured"), false}});
    QVERIFY(!expired->isVisible());
}

void RemoteControlViewTest::signInFormIsAnExplicitSecondaryFlow() {
    RemoteControlView view;
    view.resize(1100, 720);
    view.show();
    QTest::qWait(1);

    auto* signInPrompt =
        view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInButton"));
    auto* back = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInBackButton"));
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

    auto* prompt = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInButton"));
    auto* add = view.findChild<QAbstractButton*>(QStringLiteral("remotePairButton"));
    auto* dialog = view.findChild<QDialog*>(QStringLiteral("remotePairingDialog"));
    auto* qr = view.findChild<QLabel*>(QStringLiteral("remotePairingQr"));
    auto* close = view.findChild<QAbstractButton*>(QStringLiteral("remotePairingCloseButton"));
    auto* pairTitle = view.findChild<QLabel*>(QStringLiteral("remotePairingState"));
    auto* pairDialogTitle = view.findChild<QLabel*>(QStringLiteral("remotePairingDialogTitle"));
    auto* pairHelp = view.findChild<QLabel*>(QStringLiteral("remotePairingHelp"));
    auto* pairTimer = view.findChild<QLabel*>(QStringLiteral("remotePairingTimer"));
    auto* pairSteps = view.findChild<QLabel*>(QStringLiteral("remotePairingSteps"));
    auto* pairCode = view.findChild<QLabel*>(QStringLiteral("remotePairingConfirmationCode"));
    auto* pairAction = view.findChild<QAbstractButton*>(QStringLiteral("remoteApproveButton"));
    auto* pairCancel =
        view.findChild<QAbstractButton*>(QStringLiteral("remotePairingCancelButton"));
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
    auto* prompt = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInPromptButton"));
    auto* signIn = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignInButton"));
    auto* signOut = view.findChild<QAbstractButton*>(QStringLiteral("remoteSignOutButton"));
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
