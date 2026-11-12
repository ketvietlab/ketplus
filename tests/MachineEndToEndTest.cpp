#include "FakeCodex.h"
#include "KetSidebarTree.h"
#include "SshFixture.h"
#include "SyntheticRelay.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "machine/MachineWorkspaceView.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachines.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/LinuxRelayMachines.h"
#include "remote/LinuxSshMachines.h"
#include "remote/RemoteControlView.h"
#include "ssh/SshMachineBackend.h"
#include "ssh/SshMachineStore.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

// #419 end to end (ADR 0119): Connections connects a Linux machine, the picker selects it and
// the window's sidebar, Agent Workspace, worktree picker, composer and approval cards follow its
// MachineBackend. Over SSH this is a real ketplus_daemon behind a loopback sshd, reached through
// SshTunnel; the provider is FakeCodex, so no provider quota is used. Over the Relay it is the
// loopback relay of SyntheticRelay.h with its synthetic host, so no real relay or host is used.
using namespace ketplus;
using namespace ketplus::remote::ssh;
using namespace ketplus::remote::ssh::testing;
using State = ketplus::remote::MachineBackend::ConnectionState;

namespace {

QStringList sidebarTitles(const MainWindow& window) {
    QStringList titles;
    for (const auto* row : window.findChildren<ket::SidebarRow*>())
        if (row->isVisibleTo(&window))
            titles.append(row->text());
    return titles;
}

bool shown(const QWidget* widget) { return widget != nullptr && !widget->isHidden(); }

QStringList transcriptRows(const MachineWorkspaceView& view) {
    QStringList rows;
    auto* list = view.findChild<QListWidget*>("machineTranscript");
    for (int i = 0; list != nullptr && i < list->count(); ++i)
        rows.append(list->item(i)->text());
    return rows;
}

QByteArray readToken(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

// A real ketplus_daemon on the fixture's socket, with FakeCodex as its provider.
class LoopbackDaemon {
  public:
    explicit LoopbackDaemon(const SshFixture& fixture) : fixture_(fixture) {}
    ~LoopbackDaemon() { stop(); }

    bool start() {
        if (!QFile::exists(fixture_.path("fake-codex")) &&
            !writeFakeCodex(fixture_.path("fake-codex"), fixture_.path("provider.log")))
            return false;
        const QString data = fixture_.path("data");
        QFile::remove(fixture_.path("local.token"));
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_DAEMON_DATA_DIR", data);
        process_.setProcessEnvironment(environment);
        process_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process_.start(KETPLUS_TEST_DAEMON,
                       {"--socket", fixture_.path("daemon.sock"), "--state",
                        QDir(data).filePath("daemon/journal.sqlite3"), "--token-file",
                        fixture_.path("local.token"), "--codex", fixture_.path("fake-codex")});
        if (!process_.waitForStarted(5000))
            return false;
        return QTest::qWaitFor(
            [this] {
                return QFile::exists(fixture_.path("daemon.sock")) &&
                       !readToken(fixture_.path("local.token")).isEmpty();
            },
            patience);
    }

    void stop() {
        if (process_.state() == QProcess::NotRunning)
            return;
        process_.terminate();
        if (!process_.waitForFinished(5000))
            process_.kill();
        process_.waitForFinished(5000);
    }

  private:
    const SshFixture& fixture_;
    QProcess process_;
};

QComboBox* machinePicker(ActivityBar* bar) {
    return bar->findChild<QComboBox*>("productMachineSelect");
}

// The user picks a machine in the activity bar's picker.
bool pick(ActivityBar* bar, const QString& machineId) {
    auto* picker = machinePicker(bar);
    const int index = picker != nullptr ? picker->findData(machineId) : -1;
    if (index < 0)
        return false;
    picker->setCurrentIndex(index);
    return true;
}

// With KETPLUS_E2E_SCREENSHOTS set to a folder, saves what the user sees for the PR.
void screenshot(QWidget* widget, const QString& name) {
    const QString folder = qEnvironmentVariable("KETPLUS_E2E_SCREENSHOTS");
    if (folder.isEmpty() || widget == nullptr)
        return;
    // A toast names this run's temporary folder; it is not part of the flow.
    for (auto* child : widget->findChildren<QWidget*>())
        if (QByteArray(child->metaObject()->className()).contains("Toast"))
            child->hide();
    QTest::qWait(100);
    widget->grab().save(QDir(folder).filePath(name + ".png"));
}

// The Connections page as the window shows it, with this flow's Linux machine rows.
void screenshotConnections(LinuxMachines* machines, const QString& name) {
    if (qEnvironmentVariable("KETPLUS_E2E_SCREENSHOTS").isEmpty())
        return;
    RemoteControlView page;
    page.linuxMachines()->setMachines(machines);
    page.resize(1100, 1400);
    page.show();
    QTest::qWait(300);
    screenshot(&page, name);
}

// The picker's open list, as the user sees it.
void screenshotPicker(ActivityBar* bar, const QString& name) {
    if (qEnvironmentVariable("KETPLUS_E2E_SCREENSHOTS").isEmpty())
        return;
    auto* picker = machinePicker(bar);
    picker->showPopup();
    QTest::qWait(200);
    screenshot(picker->view()->window(), name);
    picker->hidePopup();
}

// One listed host's Relay route over the loopback relay, as SystemRelayLink wraps it.
class LoopbackRelayLink final : public LinuxRelayLink {
  public:
    LoopbackRelayLink(ControlV2Client* client, const QString& relayId, const QString& outbox)
        : backend_(new RelayMachineBackend(client, relayId, fastOptions(outbox), this)) {
        connect(backend_, &MachineBackend::connectionStateChanged, this,
                &LinuxRelayLink::pendingChanged);
        connect(backend_, &RelayMachineBackend::commandCompleted, this,
                &LinuxRelayLink::pendingChanged);
    }
    remote::MachineBackend* backend() override { return backend_; }
    void connectToRelay() override { backend_->connectToRelay(); }
    void disconnectFromRelay() override { backend_->disconnectFromRelay(); }
    [[nodiscard]] int pendingCommands() const override {
        return static_cast<int>(backend_->pendingCommands().size());
    }

  private:
    RelayMachineBackend* backend_;
};

} // namespace

class MachineEndToEndTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir settings_;
    QTemporaryDir workspace_;

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid() && workspace_.isValid());
        // Application data (repository metadata, stores) stays out of the user's folders.
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        // This Mac's daemon is never this user's running one.
        qputenv("KETPLUS_DAEMON_DATA_DIR", QDir(settings_.path()).filePath("daemon").toUtf8());
        qputenv("KETPLUS_DAEMON_SOCKET", QDir(settings_.path()).filePath("no-daemon").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES",
                QDir(settings_.path()).filePath("conversation-worktrees.sqlite").toUtf8());
    }

    void aLinuxMachineOverSshDrivesTheWindow() {
        SshFixture fixture;
        if (!fixture.start(true, false))
            QSKIP(qPrintable(fixture.skip));
        fixture.trust(fixture.hostKey);
        const QString worktree = fixture.path("worktree");
        QVERIFY(QDir().mkpath(worktree));
        const QString canonical = QFileInfo(worktree).canonicalFilePath();
        LoopbackDaemon daemon(fixture);
        QVERIFY(daemon.start());

        // Connections: the saved machine, its SSH route over the loopback sshd, and its row.
        const SshTarget target{"fixture", {}, 0};
        {
            SshMachineStore saved(fixture.path("machines.json"));
            QVERIFY(saved.add(target));
        }
        // The app's one set of machines, as main() makes it, over the fixture's sshd.
        LinuxMachines machines;
        machines.attachSshBackend(
            fixture.path("machines.json"), [&fixture](const SshTarget& machine) {
                return std::make_unique<SshTunnelLink>(machine, fixture.options());
            });
        LinuxMachineModel& model = *machines.model();
        LinuxSshMachines& ssh = *machines.ssh_;
        QStringList pickerLabels;
        QStringList listed;
        connect(&machines, &LinuxMachines::machinesChanged, this,
                [&](const QStringList& ids, const QStringList& names) {
                    listed = ids;
                    pickerLabels = names;
                });

        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.resize(1100, 760);
        window.show();
        auto* bar = window.findChild<ActivityBar*>();
        QVERIFY(bar != nullptr);
        MachineSelection* selection = window.machineComposition()->selection();
        // The window's own wiring: its Connections, picker and MachineSelection read the set.
        window.useLinuxMachines(&machines);
        auto* connections = window.findChild<LinuxMachinesSection*>();
        QVERIFY(connections != nullptr);

        // The saved row's Connect, as the user presses it: its daemon has not named itself yet.
        QCOMPARE(model.machines().size(), 1);
        model.requestConnect(model.machines().first().id);
        const auto sshState = [&] {
            QString text = "listed=" + listed.join(',') + " picker=" + pickerLabels.join(',');
            for (const auto& id : std::as_const(listed))
                if (const auto* machine = model.machine(id))
                    text += " error=" + LinuxMachineModel::errorMessage(*machine);
            return text;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(!listed.isEmpty() && ssh.backend(listed.first()) != nullptr &&
                                      ssh.backend(listed.first())->connectionState() ==
                                          State::Connected,
                                  qPrintable(sshState()), patience);
        const QString machineId = listed.first();
        // The machine is keyed by the daemon's identity, as ADR 0119 says.
        QCOMPARE(ssh.backend(machineId)->machineId(), machineId);
        SshMachineBackend* backend = ssh.backend(machineId);
        // The Connections row and the picker say SSH and Connected.
        QTRY_VERIFY(pickerLabels.size() == 1 && pickerLabels.first().contains("SSH") &&
                    pickerLabels.first().contains("Connected"));
        const auto rowState = [connections] {
            auto* state = connections->findChild<QLabel*>("linuxMachineState");
            return state != nullptr ? state->text() : QString{};
        };
        QTRY_COMPARE(rowState(), LinuxMachineModel::stateText(LinuxMachine::State::Connected));
        screenshotConnections(&machines, "ssh-connections");

        // A conversation already on the host, started there earlier.
        struct Started {
            bool done{false};
            QString error;
            QJsonObject result;
        } seeded;
        backend->startTurn({}, -1, {{"worktree_path", worktree}, {"prompt", "continue"}},
                           [&seeded](const QJsonObject& result, const QString& error) {
                               seeded = {true, error, result};
                           });
        QTRY_VERIFY_WITH_TIMEOUT(seeded.done, patience);
        QVERIFY2(seeded.error.isEmpty(), qPrintable(seeded.error));
        const QString seededId = seeded.result.value("session_id").toString();

        // The picker selects the machine: its sidebar lists its repository, worktree and
        // conversation, and the Agent Workspace is the machine's.
        QVERIFY(pick(bar, machineId));
        MachineWorkspaceView* view = window.machineComposition()->existingView();
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto& repository : window.machineComposition()->sidebarRepositories())
                    for (const auto& conversation : repository.conversations)
                        if (conversation.toMap().value("id").toString() == seededId ||
                            conversation.toMap().value("session_id").toString() == seededId)
                            return true;
                return false;
            }(),
            patience);
        const auto repositories = window.machineComposition()->sidebarRepositories();
        QCOMPARE(repositories.size(), 1);
        // The machine's repositories are listed, so the sidebar does not call them unavailable.
        QTRY_VERIFY(!window.findChild<QLabel*>("productMachineNotice")->isVisibleTo(&window));
        QCOMPARE(QFileInfo(repositories.first().path).canonicalFilePath(), canonical);

        // A new conversation in the remote worktree; it asks for an approval.
        view->startNewConversation(repositories.first().path);
        auto* picker = view->findChild<QComboBox*>("machineWorktreePicker");
        QVERIFY(shown(picker));
        QTRY_VERIFY(picker->count() >= 1);
        int index = -1;
        for (int i = 0; i < picker->count(); ++i)
            if (QFileInfo(picker->itemData(i).toString()).canonicalFilePath() == canonical)
                index = i;
        QVERIFY2(index >= 0, "the remote worktree is not in the worktree picker");
        picker->setCurrentIndex(index);
        auto* composer = view->findChild<QPlainTextEdit*>("machineComposer");
        auto* send = view->findChild<QPushButton*>("machineSend");
        QTRY_VERIFY(composer->isEnabled() && send->isEnabled());
        QList<QJsonObject> liveEvents;
        connect(backend, &remote::MachineBackend::eventReceived, this,
                [&liveEvents](const QJsonObject& event) { liveEvents.append(event); });
        composer->setPlainText("approve");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            !view->openSessionId().isEmpty() && view->openSessionId() != seededId, patience);
        const QString created = view->openSessionId();

        // The approval card answers through the machine's backend; the transcript updates.
        QPushButton* approve = nullptr;
        // On a timeout: the live events of the new conversation and what the view shows.
        const auto approvalState = [&] {
            QStringList seen;
            for (const auto& event : std::as_const(liveEvents))
                if (event.value("session_id").toString() == created)
                    seen.append(event.value("payload").toObject().value("event_type").toString());
            auto* cards = view->findChild<QWidget*>("machineApprovals");
            return "events=" + seen.join(',') + " transcript=" + transcriptRows(*view).join('|') +
                   " cards=" + (cards && cards->isVisibleTo(view) ? "shown" : "hidden");
        };
        QTRY_VERIFY2_WITH_TIMEOUT((approve = view->findChild<QPushButton*>("machineApprove")) !=
                                          nullptr &&
                                      approve->isVisibleTo(view) && approve->isEnabled(),
                                  qPrintable(approvalState()), patience);
        approve->click();
        QTRY_VERIFY2_WITH_TIMEOUT(transcriptRows(*view).join('\n').contains("finished"),
                                  qPrintable(approvalState()), patience);
        screenshot(&window, "ssh-window");
        screenshotPicker(bar, "ssh-picker");
        QTRY_VERIFY_WITH_TIMEOUT(
            view->findChild<QPushButton*>("machineApprove") == nullptr ||
                !view->findChild<QPushButton*>("machineApprove")->isVisibleTo(view),
            patience);
        QCOMPARE(view->openSessionId(), created);

        // Offline mid-session: the daemon stops. The picker, the row and the composer say so,
        // and the window never shows This Mac's lists instead.
        daemon.stop();
        QTRY_VERIFY_WITH_TIMEOUT(backend->connectionState() != State::Connected, patience);
        // ADR 0119: a route that lost its host is Reconnecting, then Offline.
        const auto down = [](const QString& text) {
            return text.contains("Reconnecting") || text.contains("Offline");
        };
        QTRY_VERIFY_WITH_TIMEOUT(pickerLabels.size() == 1 && down(pickerLabels.first()) &&
                                     down(machinePicker(bar)->currentText()),
                                 patience);
        QTRY_VERIFY(down(rowState()));
        QTRY_VERIFY(!composer->isEnabled());
        auto* reason = view->findChild<QLabel*>("machineComposerReason");
        QVERIFY(!reason->text().isEmpty());
        QCOMPARE(selection->machineId(), machineId);
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));

        // The daemon returns: the route recovers by itself and the composer is usable again.
        QVERIFY(daemon.start());
        QTRY_VERIFY_WITH_TIMEOUT(backend->connectionState() == State::Connected, patience);
        QTRY_VERIFY(pickerLabels.size() == 1 && pickerLabels.first().contains("Connected"));
        QTRY_COMPARE(rowState(), LinuxMachineModel::stateText(LinuxMachine::State::Connected));
        QTRY_VERIFY_WITH_TIMEOUT(composer->isEnabled(), patience);
        QCOMPARE(view->openSessionId(), created);

        // This Mac again: the local Agent Workspace and sidebar, none of the machine's rows.
        QVERIFY(pick(bar, "local"));
        QCOMPARE(selection->machineId(), QStringLiteral("local"));
        QVERIFY(!shown(view));
        QVERIFY(shown(window.findChild<AgentWorkspaceView*>()));
        QVERIFY(window.machineComposition()->sidebarRepositories().isEmpty());
        QTRY_VERIFY(!sidebarTitles(window).join('\n').contains("approve"));
    }

    void aLinuxMachineOverTheRelayDrivesTheWindow() {
        r03::resetCredentials();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RemoteControlTestAccess::restorePoll(client, 10);
        // The host answers transcript reads with a short window and pending approvals, approval
        // answers and session creation in the daemon's result shapes.
        RestoreHost restore(fixture);
        restore.clientSigning = relay.clientSigning;
        restore.pages = [&restore](const QJsonObject&) {
            return QJsonObject{
                {"ok", true},
                {"transcript_window_version", 1},
                {"session_id", restore.session},
                {"session_generation", restore.generation},
                {"snapshot_cursor", 9},
                {"projection_revision", 1},
                {"before_cursor", 3},
                {"has_more_before", false},
                {"events", QJsonArray{restore.event(3, 1, {{"text", "relay history prompt"}},
                                                    "user.message")}}};
        };
        relay.restore = &restore;
        bool approved = false;
        // The host's events for opaque-session follow the restored window (cursor 9).
        int cursor = 9;
        relay.results = [&](const QString& kind, const QJsonObject&) -> QJsonObject {
            if (kind == "session.pending_approvals")
                return {{"pending_approvals",
                         approved ? QJsonArray{}
                                  : QJsonArray{QJsonObject{{"approval_id", "approval-1"},
                                                           {"kind", "command"},
                                                           {"revision", 1},
                                                           {"command", "make test"}}}}};
            if (kind == "approval.resolve") {
                approved = true;
                relay.events.append(
                    fixture.event(++cursor, "approval.resolved", {{"approval_id", "approval-1"}}));
                return {{"accepted", kind}};
            }
            if (kind == "session.start_turn") {
                relay.events.append(fixture.event(++cursor));
                return {{"turn_sequence", 2}};
            }
            if (kind == "session.create")
                return {{"session_id", "created-session"}, {"generation", 3}, {"turn_sequence", 1}};
            return {};
        };
        // The host is away at first: the worktrees are listed but not yet verified by it.
        relay.hostPaused = true;

        // Connections: the Relay lists the host, the account is entitled, the link runs.
        // The app's one set of machines, as main() makes it: its session is entitled to the
        // Relay, and each listed host's link runs over the loopback relay.
        LinuxMachines machines;
        RemoteControlTestAccess::paired(*machines.client());
        RemoteControlTestAccess::entitle(*machines.client(), QJsonArray{"ketplus.relay"});
        machines.loading_ = true; // the listing comes from this flow, not the network
        machines.attachRelayBackend([&client, &dir](const QString& relayId) {
            return std::make_unique<LoopbackRelayLink>(&client, relayId,
                                                       dir.filePath(relayId + ".outbox.json"));
        });
        LinuxMachineModel& model = *machines.model();
        // As in the window, where SSH and the Relay are attached together.
        model.setBackendsAttached(true);
        QVERIFY(model.relayEntitled());
        LinuxRelayMachines& relayMachines = *machines.relay_;
        QStringList pickerLabels;
        QStringList listed;
        connect(&machines, &LinuxMachines::machinesChanged, this,
                [&](const QStringList& ids, const QStringList& names) {
                    listed = ids;
                    pickerLabels = names;
                });

        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.resize(1100, 760);
        window.show();
        auto* bar = window.findChild<ActivityBar*>();
        QVERIFY(bar != nullptr);
        MachineSelection* selection = window.machineComposition()->selection();
        window.useLinuxMachines(&machines);
        auto* connections = window.findChild<LinuxMachinesSection*>();
        QVERIFY(connections != nullptr);

        const QString machineId = CloudCrypto::keyFingerprint(fixture.hostKeys);
        relayMachines.setListed(
            {{"linux-host", machineId, "builder", QDateTime::currentDateTimeUtc()}});
        relayMachines.setEnabled(true);
        QTRY_VERIFY(listed == QStringList{machineId} && relayMachines.backend(machineId) &&
                    relayMachines.backend(machineId)->connectionState() == State::Connected);
        auto* backend = relayMachines.backend(machineId);
        QCOMPARE(backend->machineId(), machineId);
        QTRY_VERIFY(pickerLabels.size() == 1 && pickerLabels.first().contains("Relay") &&
                    pickerLabels.first().contains("Connected"));
        const auto rowState = [connections] {
            auto* state = connections->findChild<QLabel*>("linuxMachineState");
            return state != nullptr ? state->text() : QString{};
        };
        QTRY_COMPARE(rowState(), LinuxMachineModel::stateText(LinuxMachine::State::Connected));
        screenshotConnections(&machines, "relay-connections");

        // The picker selects the machine. Its worktree is pending until the host verifies it.
        QVERIFY(pick(bar, machineId));
        MachineWorkspaceView* view = window.machineComposition()->existingView();
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));
        view->startNewConversation();
        auto* picker = view->findChild<QComboBox*>("machineWorktreePicker");
        QVERIFY(picker != nullptr);
        const auto pickable = [picker](const QString& id) {
            const int index = picker->findData(id);
            auto* items = qobject_cast<QStandardItemModel*>(picker->model());
            return index >= 0 && items != nullptr && items->item(index)->isEnabled();
        };
        const auto pickerItems = [picker] {
            QStringList items;
            for (int i = 0; i < picker->count(); ++i)
                items.append(picker->itemText(i) + "=" + picker->itemData(i).toString());
            return items.join(',');
        };
        QTRY_VERIFY2_WITH_TIMEOUT(pickerItems().contains("verifying"), qPrintable(pickerItems()),
                                  patience);
        // wt-a holds a conversation whose metadata the host sealed; wt-empty is only in the
        // relay's index, so it waits for the host to sign it.
        QVERIFY(!pickable("wt-empty"));
        // The host returns: the verification lands and the worktree becomes selectable.
        relay.hostPaused = false;
        QTRY_VERIFY2_WITH_TIMEOUT(pickable("wt-empty"), qPrintable(pickerItems()), patience);
        QVERIFY(!pickerItems().contains("verifying"));
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto& repository : window.machineComposition()->sidebarRepositories())
                    for (const auto& conversation : repository.conversations)
                        if (conversation.toMap().value("id").toString() == "opaque-session")
                            return true;
                return false;
            }(),
            patience);

        // The listed conversation opens with its transcript and its pending approval.
        view->openConversation("opaque-session");
        QTRY_COMPARE(view->openSessionId(), QStringLiteral("opaque-session"));
        QTRY_VERIFY_WITH_TIMEOUT(transcriptRows(*view).join('\n').contains("relay history prompt"),
                                 patience);
        QPushButton* approve = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((approve = view->findChild<QPushButton*>("machineApprove")) !=
                                         nullptr &&
                                     approve->isVisibleTo(view) && approve->isEnabled(),
                                 patience);
        approve->click();
        QTRY_VERIFY_WITH_TIMEOUT(approved, patience);
        QTRY_VERIFY_WITH_TIMEOUT(
            view->findChild<QPushButton*>("machineApprove") == nullptr ||
                !view->findChild<QPushButton*>("machineApprove")->isVisibleTo(view),
            patience);
        // A turn: the host's events reach the transcript through the Relay's event poll.
        auto* composer = view->findChild<QPlainTextEdit*>("machineComposer");
        auto* send = view->findChild<QPushButton*>("machineSend");
        QTRY_VERIFY(composer->isEnabled() && send->isEnabled());
        composer->setPlainText("continue over the relay");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(transcriptRows(*view).join('\n').contains("private fixture text"),
                                 patience);
        screenshot(&window, "relay-window");
        screenshotPicker(bar, "relay-picker");
        // The relay only ever saw ciphertext.
        for (const auto& body : std::as_const(network.bodies))
            QVERIFY(!body.contains("continue over the relay"));

        // A new conversation queued while the host is away, across a relay outage.
        relay.hostPaused = true;
        view->startNewConversation();
        QTRY_VERIFY(pickable("wt-a"));
        picker->setCurrentIndex(picker->findData("wt-a"));
        QTRY_VERIFY(composer->isEnabled() && send->isEnabled());
        composer->setPlainText("queued over the relay");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const auto pending = qobject_cast<RelayMachineBackend*>(backend)->pendingCommands();
                for (const auto& id : pending)
                    if (relay.commands.value(id).value("kind") == "session.create")
                        return true;
                return false;
            }(),
            patience);
        QVERIFY(view->openSessionId().isEmpty());

        // Offline mid-session: the relay stops answering. The picker, the row and the composer
        // say so, and the window never shows This Mac's lists instead.
        relay.offline = true;
        QTRY_VERIFY_WITH_TIMEOUT(backend->connectionState() != State::Connected, patience);
        // ADR 0119: a route that lost its host is Reconnecting, then Offline.
        const auto down = [](const QString& text) {
            return text.contains("Reconnecting") || text.contains("Offline");
        };
        QTRY_VERIFY_WITH_TIMEOUT(pickerLabels.size() == 1 && down(pickerLabels.first()) &&
                                     down(machinePicker(bar)->currentText()),
                                 patience);
        QTRY_VERIFY(down(rowState()));
        QTRY_VERIFY(!composer->isEnabled());
        auto* reason = view->findChild<QLabel*>("machineComposerReason");
        QVERIFY(!reason->text().isEmpty());
        QCOMPARE(selection->machineId(), machineId);
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));

        // The relay and the host return: the route recovers by itself, and the queued create
        // completes through commandCompleted and opens the new conversation.
        relay.addSession("created-session");
        restore.session = "created-session";
        relay.offline = false;
        relay.hostPaused = false;
        QTRY_VERIFY_WITH_TIMEOUT(backend->connectionState() == State::Connected, patience);
        QTRY_VERIFY(pickerLabels.size() == 1 && pickerLabels.first().contains("Connected"));
        QTRY_COMPARE(rowState(), LinuxMachineModel::stateText(LinuxMachine::State::Connected));
        QTRY_COMPARE_WITH_TIMEOUT(view->openSessionId(), QStringLiteral("created-session"),
                                  patience);
        QTRY_VERIFY(qobject_cast<RelayMachineBackend*>(backend)->pendingCommands().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(composer->isEnabled(), patience);

        // This Mac again.
        QVERIFY(pick(bar, "local"));
        QCOMPARE(selection->machineId(), QStringLiteral("local"));
        QVERIFY(!shown(view));
        QVERIFY(shown(window.findChild<AgentWorkspaceView*>()));
        QVERIFY(window.machineComposition()->sidebarRepositories().isEmpty());
    }
};

QTEST_MAIN(MachineEndToEndTest)
#include "MachineEndToEndTest.moc"
