#include "RunnerDesktopRig.h"

#include "cloud-privacy/MemoryCredentials.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QTest>

namespace runner_desktop_test {

namespace {

constexpr int pairingWaitMs = 45'000; // the desktop polls the ticket at 2 s, then backs off
constexpr int onlineWaitMs = 120'000; // start-up clean-up is bounded at 90 s

bool git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("git"), arguments);
    return process.waitForFinished(30'000) && process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

bool write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

// The addresses of /proc/net/tcp{,6} rows, by socket inode: "local remote" in kernel hex.
QHash<QString, QString> tcpRows() {
    QHash<QString, QString> rows;
    for (const QString& name :
         {QStringLiteral("/proc/net/tcp"), QStringLiteral("/proc/net/tcp6")}) {
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        (void)file.readLine();
        while (!file.atEnd()) {
            const QStringList fields =
                QString::fromLatin1(file.readLine()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (fields.size() > 9)
                rows.insert(fields.at(9), fields.at(1) + QLatin1Char(' ') + fields.at(2));
        }
    }
    return rows;
}

bool loopbackHex(const QString& address) {
    const QString host = address.section(QLatin1Char(':'), 0, 0);
    if (host.size() == 8) // 127.0.0.0/8, little-endian
        return host.endsWith(QStringLiteral("7F")) || host == QStringLiteral("00000000");
    // ::1, :: or ::ffff:127.x.x.x
    return host == QStringLiteral("00000000000000000000000001000000") ||
           host == QStringLiteral("00000000000000000000000000000000") ||
           (host.startsWith(QStringLiteral("0000000000000000FFFF0000")) &&
            host.endsWith(QStringLiteral("7F")));
}

} // namespace

int processesWith(const QByteArray& marker) {
    int found = 0;
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs);
    for (const QString& entry : entries) {
        bool number = false;
        (void)entry.toLongLong(&number);
        if (!number)
            continue;
        QFile file(QStringLiteral("/proc/%1/cmdline").arg(entry));
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(marker))
            ++found;
    }
    return found;
}

QStringList jobUnits() {
    QStringList units;
    QDirIterator it(QStringLiteral("/sys/fs/cgroup/user.slice"), {QStringLiteral("ketplus-job-*")},
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString unit = QFileInfo(it.next()).fileName();
        // Only the units of this build's executor: other work on the machine has its own.
        QProcess show;
        show.start(QStringLiteral("systemctl"),
                   {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("--value"),
                    QStringLiteral("-p"), QStringLiteral("ExecStart"), unit});
        if (!show.waitForFinished(10'000) ||
            show.readAllStandardOutput().contains(QByteArray(KETPLUS_JOB_EXECUTOR_PATH)))
            units.append(unit);
    }
    units.sort();
    return units;
}

QByteArray sleepMarker() {
    return "4000." +
           QByteArray::number(QRandomGenerator::global()->bounded(100'000'000, 999'999'999));
}

QStringList nonLoopbackTcp(const qint64 pid) {
    QStringList found;
    const QHash<QString, QString> rows = tcpRows();
    const QDir fds(QStringLiteral("/proc/%1/fd").arg(pid));
    for (const QString& fd : fds.entryList(QDir::System | QDir::NoDotAndDotDot)) {
        const QString target = QFile::symLinkTarget(fds.filePath(fd));
        static const QRegularExpression socket(QStringLiteral("socket:\\[(\\d+)\\]"));
        const auto match = socket.match(target);
        if (!match.hasMatch() || !rows.contains(match.captured(1)))
            continue;
        const QString row = rows.value(match.captured(1));
        if (!loopbackHex(row.section(QLatin1Char(' '), 0, 0)) ||
            !loopbackHex(row.section(QLatin1Char(' '), 1, 1)))
            found.append(row);
    }
    return found;
}

// The template of a rig's directory, below a root made first.
QString rigTemplate() {
    QDir().mkpath(QStringLiteral(KETPLUS_E2E_ROOT));
    return QDir(QStringLiteral(KETPLUS_E2E_ROOT)).filePath(QStringLiteral("rig-XXXXXX"));
}

Rig::Rig() : m_directory(rigTemplate()) {
    if (!m_directory.isValid())
        return;
    // The daemon's credentials are one store per process: each scenario starts with none.
    r03::resetCredentials();
    root = m_directory.path();
    trace = QDir(root).filePath(QStringLiteral("trace"));
    QDir().mkpath(trace);
    relay.strict = true;
    relay.machineToken = DesktopEnd::machineToken;
    relay.machineId = DesktopEnd::machineId;
    relay.workspaceId = DesktopEnd::workspaceId;
    // Only the desktop approves.
    relay.approval = runner_app_test::FakeRelay::Approval::Never;
    desktop = std::make_unique<DesktopEnd>(relay.url(), QDir(root).filePath("desktop"), root);
    auto& keys = relay.desktop(DesktopEnd::machineId);
    keys.keyVersion = 1;
    keys.signingPublicKey = desktop->signingPublicKey();
    keys.agreementPublicKey = desktop->agreementPublicKey();
    keys.signingPrivateKey.clear();
    runner =
        std::make_unique<RunnerProcess>(QStringLiteral(KETPLUS_RUNNER_PATH), root, relay.url());
    m_ready =
        keys.signingPublicKey.size() == 32 && keys.agreementPublicKey.size() == 32 &&
        QTest::qWaitFor([this] { return desktop->socketOpen() && relay.desktopOnline(); }, 10'000);
}

Rig::~Rig() {
    if (runner)
        (void)runner->stop(30'000);
    // The runner's own output stays in the build tree when the scenario failed.
    if (runner && QTest::currentTestFailed()) {
        QFile file(
            QDir(QStringLiteral(KETPLUS_E2E_ROOT))
                .filePath(QString::fromLatin1(QTest::currentTestFunction()) + ".runner.log"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(runner->log());
    }
    runner.reset();
    desktop.reset();
}

Rig::Pairing Rig::pair(const std::function<QString(const QString&)>& codeFor) {
    Pairing result;
    const QJsonObject created = desktop->call(QStringLiteral("runner.pairing.create"));
    result.ticketId = created.value("ticket_id").toString();
    const QString token = created.value("token").toString();
    if (result.ticketId.isEmpty() || token.isEmpty() ||
        !runner->startRegister(token.toUtf8(), {QStringLiteral("e2e")}))
        return result;
    result.runnerCode = runner->waitCode(pairingWaitMs);
    QJsonObject status;
    (void)QTest::qWaitFor(
        [&] {
            status = desktop->call(QStringLiteral("runner.pairing.status"),
                                   {{"ticket_id", result.ticketId}});
            return status.value("state").toString() != QStringLiteral("waiting") &&
                   !status.value("confirmation_code").toString().isEmpty();
        },
        pairingWaitMs);
    result.desktopCode = status.value("confirmation_code").toString();
    result.desktopState =
        QStringList{status.value("state").toString(), status.value("failure_reason").toString(),
                    status.value("last_error").toString(), status.value("error_code").toString()}
            .join(QLatin1Char('/'));
    result.runnerId = status.value("runner").toObject().value("runner_id").toString();
    if (result.desktopCode.isEmpty())
        return result;
    result.approveCode = codeFor ? codeFor(result.desktopCode) : result.desktopCode;
    const QJsonObject approved =
        desktop->call(QStringLiteral("runner.pairing.approve"),
                      {{"ticket_id", result.ticketId}, {"code", result.approveCode}});
    result.approveError = approved.value("ok").toBool()
                              ? QString()
                              : approved.value("error_code").toString(QStringLiteral("unknown"));
    if (result.approveError.isEmpty()) {
        int code = -1;
        if (runner->waitExit(pairingWaitMs, &code))
            result.registerExit = code;
    }
    return result;
}

QString Rig::pairAndRun() {
    const Pairing pairing = pair();
    if (pairing.runnerId.isEmpty() || !pairing.approveError.isEmpty() ||
        pairing.registerExit != 0 || pairing.runnerCode != pairing.desktopCode ||
        !runner->startRun())
        return {};
    if (!QTest::qWaitFor([&] { return relay.runnerOnline(pairing.runnerId) || !runner->running(); },
                         onlineWaitMs) ||
        !runner->running())
        return {};
    return pairing.runnerId;
}

QString Rig::repository(const QMap<QString, QByteArray>& committed,
                        const QMap<QString, QByteArray>& working) {
    const QString path = QDir(root).filePath(QStringLiteral("repo-%1").arg(++m_repositories));
    QDir().mkpath(path);
    bool ok = git(path, {"init", "-q", "-b", "main"}) &&
              git(path, {"config", "user.email", "e2e@example.invalid"}) &&
              git(path, {"config", "user.name", "E2E"});
    for (auto file = committed.cbegin(); ok && file != committed.cend(); ++file)
        ok = write(QDir(path).filePath(file.key()), file.value());
    ok = ok && git(path, {"add", "-A"}) && git(path, {"commit", "-q", "-m", "base"});
    for (auto file = working.cbegin(); ok && file != working.cend(); ++file)
        ok = write(QDir(path).filePath(file.key()), file.value());
    return ok ? path : QString();
}

QString Rig::submit(const QString& shell, const QString& cwd, const qint64 timeoutMs,
                    const QStringList& runsOn, QString* code) {
    QString answer;
    const QString taskId =
        desktop->submit(shell, cwd, runsOn, timeoutMs, {{QStringLiteral("TRACE"), trace}}, &answer);
    if (code)
        *code = answer;
    if (!answer.isEmpty()) {
        qWarning("submit did not reach the remote jobs: %s", qPrintable(answer));
        return {};
    }
    return taskId;
}

std::optional<RemoteJobRecord> Rig::waitTerminal(const QString& taskId, const int timeoutMs) {
    std::optional<RemoteJobRecord> record;
    if (!QTest::qWaitFor(
            [&] {
                record = desktop->record(taskId);
                return record && record->terminal();
            },
            timeoutMs))
        return std::nullopt;
    return record;
}

bool Rig::waitOutput(const QString& taskId, const QByteArray& part, const int timeoutMs) {
    return QTest::qWaitFor([&] { return desktop->output(taskId).contains(part); }, timeoutMs);
}

QString Rig::trail() const {
    QStringList lines;
    for (const auto& request : relay.requests)
        lines.append(QString::fromLatin1(request.method) + QLatin1Char(' ') + request.path);
    lines.append(QStringLiteral("violations: ") + relay.violations.join(QStringLiteral("; ")));
    return lines.join(QStringLiteral(" | "));
}

int Rig::signalCount(const QString& from, const QString& type) const {
    int count = 0;
    for (const QJsonObject& entry : relay.socketLog) {
        const QJsonValue message = entry.value(QStringLiteral("message"));
        const QJsonObject object =
            message.isObject() ? message.toObject()
                               : QJsonDocument::fromJson(message.toString().toUtf8()).object();
        if (entry.value(QStringLiteral("from")).toString() == from &&
            object.value(QStringLiteral("type")).toString() == type)
            ++count;
    }
    return count;
}

QString Rig::problems(const QStringList& jobData) const {
    if (!relay.violations.isEmpty())
        return QStringLiteral("Relay protocol: ") + relay.violations.join(QStringLiteral("; "));
    if (desktop->refusedRequests() > 0)
        return QStringLiteral("the desktop asked a host other than the loopback Relay");
    QJsonArray log;
    for (const QJsonObject& entry : relay.socketLog)
        log.append(entry);
    const QByteArray signaling = QJsonDocument(log).toJson(QJsonDocument::Compact);
    for (const char* kind : {"typ srflx", "typ relay", "stun:", "turn:"})
        if (signaling.contains(kind))
            return QStringLiteral(
                       "signaling holds a candidate or server that is not on this host: ") +
                   QString::fromLatin1(kind);
    for (const qint64 pid : {qint64(QCoreApplication::applicationPid()), runner->pid()}) {
        if (pid <= 0)
            continue;
        const QStringList outside = nonLoopbackTcp(pid);
        if (!outside.isEmpty())
            return QStringLiteral("TCP outside the loopback: ") +
                   outside.join(QStringLiteral(", "));
    }
    const QByteArray printed = runner->log();
    for (const QString& data : jobData)
        if (!data.isEmpty() && printed.contains(data.toUtf8()))
            return QStringLiteral("the runner printed job data the test created: ") + data;
    for (const QString& secret : relay.secrets())
        if (!secret.isEmpty() && printed.contains(secret.toUtf8()))
            return QStringLiteral("the runner printed a credential");
    return {};
}

} // namespace runner_desktop_test
