// T-89: in the real window, the + on a team folder in the sidebar opens Add a member. Against a
// scripted daemon on a private socket that offers teams/1, from the editor as well. Run with
// QT_QPA_PLATFORM=offscreen.
#include "KetProfile.h"
#include "KetStyle.h"
#include "app/AgentWorktreeDialog.h"
#include "app/MainWindow.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Speaks the local framing protocol: a 4-byte big-endian length, then a JSON envelope.
class ScriptedDaemon final : public QObject {
  public:
    ScriptedDaemon(const QString& socketPath, QJsonObject teamList)
        : teamList_(std::move(teamList)) {
        QLocalServer::removeServer(socketPath);
        listening = server_.listen(socketPath);
        connect(&server_, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection())
                connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
        });
    }

    bool listening{false};

  private:
    void read(QLocalSocket* socket) {
        QByteArray& buffer = buffers_[socket];
        buffer.append(socket->readAll());
        while (buffer.size() >= 4) {
            const quint32 size = (quint32(uchar(buffer[0])) << 24U) |
                                 (quint32(uchar(buffer[1])) << 16U) |
                                 (quint32(uchar(buffer[2])) << 8U) | quint32(uchar(buffer[3]));
            if (buffer.size() < qsizetype(size) + 4)
                return;
            const QJsonObject envelope = QJsonDocument::fromJson(buffer.mid(4, size)).object();
            buffer.remove(0, qsizetype(size) + 4);
            const QString type = envelope.value("message_type").toString();
            QJsonObject payload{{"ok", true}};
            if (type == "daemon.hello")
                payload.insert("features", QJsonArray{"teams/1"});
            else if (type == "team.list")
                payload = teamList_;
            write(socket, {{"message_type", type + ".result"},
                           {"correlation_id", envelope.value("message_id")},
                           {"payload", payload}});
        }
    }
    static void write(QLocalSocket* socket, const QJsonObject& envelope) {
        const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        const auto size = quint32(body.size());
        QByteArray frame(4, Qt::Uninitialized);
        frame[0] = char((size >> 24U) & 0xffU);
        frame[1] = char((size >> 16U) & 0xffU);
        frame[2] = char((size >> 8U) & 0xffU);
        frame[3] = char(size & 0xffU);
        socket->write(frame + body);
    }

    QJsonObject teamList_;
    QLocalServer server_;
    QHash<QLocalSocket*, QByteArray> buffers_;
};

QJsonObject member(const QString& session, const QString& role, const QString& name) {
    return {{"session_id", session}, {"role", role}, {"name", name}};
}

QJsonObject team(const QString& repository) {
    return {{"id", "7"},
            {"name", "Shop"},
            {"repository", repository},
            {"members",
             QJsonArray{member("s-lead", "leader", "Lead"), member("s-dev1", "developer", "Dev 1"),
                        member("s-rev", "reviewer", "Review")}},
            {"revision", 1},
            {"open_decisions", 0}};
}

} // namespace

class TeamSidebarWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ket::Style::install(*qApp);
        ket::Profile::setCurrent(ket::Profile::ketPlus());
    }

    void theSidebarPlusOpensAddMemberInTheWindow() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/kpts-XXXXXX"));
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        const QStringList names{
            "KETPLUS_DAEMON_SOCKET",          "KETPLUS_DAEMON_STATE",
            "KETPLUS_DAEMON_TOKEN_FILE",      "KETPLUS_DISABLE_DAEMON_AUTOSTART",
            "KETPLUS_CONVERSATION_WORKTREES", "KETPLUS_DAEMON_EXECUTABLE"};
        QHash<QString, QByteArray> saved;
        for (const QString& name : names)
            saved.insert(name, qgetenv(name.toUtf8()));
        const auto restore = qScopeGuard([&] {
            for (const QString& name : names) {
                const QByteArray value = saved.value(name);
                value.isNull() ? qunsetenv(name.toUtf8()) : qputenv(name.toUtf8(), value);
            }
        });
        const QString socket = directory.filePath("d.sock");
        const QString token = directory.filePath("local.token");
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", directory.filePath("journal.sqlite3").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        // Nothing is launched (autostart is off); a named executable makes the client connect.
        qputenv("KETPLUS_DAEMON_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("worktrees.sqlite").toUtf8());
        QFile tokenFile(token);
        QVERIFY(tokenFile.open(QIODevice::WriteOnly));
        tokenFile.write(QByteArray(32, 't'));
        tokenFile.close();
        QVERIFY(tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));

        const QString repository = directory.filePath("repo");
        QVERIFY(QDir().mkpath(repository));
        QCOMPARE(QProcess::execute("git", {"-C", repository, "init", "-q"}), 0);
        const QString canonical = QFileInfo(repository).canonicalFilePath();
        ScriptedDaemon daemon(socket, {{"ok", true}, {"teams", QJsonArray{team(canonical)}}});
        QVERIFY(daemon.listening);

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        // The editor, not the agent workspace: the sidebar lists the team before any
        // conversation view exists, and its + must still lead somewhere.
        window.openFolder(repository);
        window.resize(1280, 800);
        window.show();
        QAbstractButton* add = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (add = window.findChild<QAbstractButton*>("productTeamFolderAdd")) != nullptr, 10000);
        QVERIFY(add->isEnabled());
        QVERIFY(!ketplus::agentDialogOpen(&window, QStringLiteral("addMemberDialog")));
        // Hovering the folder shows its +, as in the app.
        auto* branch = add->parentWidget();
        QEnterEvent enter(QPointF(4, 4), QPointF(4, 4), branch->mapToGlobal(QPointF(4, 4)));
        QApplication::sendEvent(branch, &enter);
        QTRY_VERIFY(add->isVisible());
        add->click();
        QTRY_VERIFY(ketplus::agentDialogOpen(&window, QStringLiteral("addMemberDialog")));
    }
};

QTEST_MAIN(TeamSidebarWindowTest)
#include "TeamSidebarWindowTest.moc"
