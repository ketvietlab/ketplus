#include <QtGlobal>
// POSIX-only; Windows builds NativeJobUnsupported.cpp instead.
#if !defined(Q_OS_WIN)
#include "runtime/executor/LinuxJobContainment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <memory>
#include <sys/wait.h>
#include <thread>
#include <utility>
#ifdef Q_OS_LINUX
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

namespace ketplus::daemon::executor {
namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(16384).trimmed() : QByteArray{};
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool run(const QString& program, const QStringList& args, QByteArray* output, QString* error,
         int timeout = 4000) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = "The user service manager did not answer within its deadline.";
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput().left(16384);
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!ok && error)
        *error = QString::fromUtf8(process.readAllStandardError().left(1024)).trimmed();
    return ok;
}
QStringList properties() {
    return {"--user",
            "--quiet",
            "--collect",
            "--property=Delegate=yes",
            "--property=KillMode=control-group",
            "--property=TimeoutStopSec=3s",
            "--property=Restart=no",
            "--property=Type=exec"};
}
QJsonObject filesystemIdentity(const QString& path) {
#ifdef Q_OS_LINUX
    const auto boot = linuxBootId();
    if (boot.isEmpty())
        return {};
    struct stat status{};
    struct statfs filesystem{};
    if (lstat(QFile::encodeName(path).constData(), &status) == 0 && S_ISDIR(status.st_mode) &&
        statfs(QFile::encodeName(path).constData(), &filesystem) == 0 &&
        filesystem.f_type == 0x63677270)
        return {{"path", path},
                {"inode", QString::number(status.st_ino)},
                {"device", QString::number(status.st_dev)},
                {"boot_id", boot}};
#else
    Q_UNUSED(path)
#endif
    return {};
}
} // namespace

QString linuxBootId() { return QString::fromLatin1(read("/proc/sys/kernel/random/boot_id")); }

QString linuxJobUnit(const QString& task, const QString& lease) {
    return "ketplus-job-" +
           QString::fromLatin1(QCryptographicHash::hash(task.toUtf8() + '\0' + lease.toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex()
                                   .left(40)) +
           ".service";
}

QJsonObject linuxUnitProperties(const QString& unit, QString* error) {
    if (!unit.startsWith("ketplus-job-") || !unit.endsWith(".service") || unit.contains('/'))
        return {};
    QByteArray output;
    // systemctl show on a missing unit still emits LoadState=not-found. An
    // inaccessible manager emits no usable identity and is never treated as gone.
    run("/usr/bin/systemctl",
        {"--user", "show", unit,
         "--property=LoadState,ActiveState,"
         "SubState,MainPID,InvocationID,ControlGroup,Delegate,KillMode,TimeoutStopUSec"},
        &output, error);
    QJsonObject result;
    for (const auto& line : output.split('\n')) {
        const auto split = line.indexOf('=');
        if (split > 0)
            result.insert(QString::fromLatin1(line.left(split)),
                          QString::fromLatin1(line.mid(split + 1)));
    }
    return result;
}

bool linuxContainmentAvailable(const QString& helper, QString* error) {
#ifdef Q_OS_LINUX
    if (geteuid() == 0 || linuxBootId().isEmpty()) {
        *error = "The Linux executor requires a non-root daemon user and a boot identity.";
        return false;
    }
    const auto task = QUuid::createUuid().toString(QUuid::Id128);
    const auto lease = QUuid::createUuid().toString(QUuid::Id128);
    auto arguments = properties();
    arguments << "--wait" << "--pipe" << "--property=RuntimeMaxSec=10s"
              << "--unit=" + linuxJobUnit(task, lease) << helper << "--linux-probe" << task
              << lease;
    QByteArray output;
    if (!run("/usr/bin/systemd-run", arguments, &output, error, 6000))
        return false;
    const auto report = QJsonDocument::fromJson(output.trimmed()).object();
    if (!report.value("available").toBool()) {
        *error = "Delegated cgroup v2, nsdelegate, isolated namespaces or user-unit cleanup "
                 "are unavailable.";
        return false;
    }
    return true;
#else
    Q_UNUSED(helper)
    *error = "Linux cgroup containment is unavailable on this platform.";
    return false;
#endif
}

namespace {
// Every daemon-side manager RPC yields to the heartbeat event loop. Synchronous
// helpers above are used only by the standalone supervisor/probe process.
void runAsync(const QString& program, const QStringList& arguments, QObject* owner, int timeout,
              std::function<void(bool, QByteArray, QString)> done) {
    auto* process = new QProcess(owner);
    process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process->setStandardInputFile(QProcess::nullDevice());
    auto* deadline = new QTimer(process);
    deadline->setSingleShot(true);
    auto completed = std::make_shared<bool>(false);
    const auto finish = [process, deadline, completed, done = std::move(done)](
                            bool success, const QByteArray& output, const QString& error) {
        if (std::exchange(*completed, true))
            return;
        deadline->stop();
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
        done(success, output, error);
    };
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    QObject::connect(
        process, &QProcess::finished, owner,
        [process, finish](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit && code == 0,
                   process->readAllStandardOutput().left(16384),
                   QString::fromUtf8(process->readAllStandardError().left(1024)).trimmed());
        });
    QObject::connect(process, &QProcess::errorOccurred, owner,
                     [finish](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                             finish(false, {}, "The user service manager could not be started.");
                     });
    QObject::connect(deadline, &QTimer::timeout, owner, [process, finish] {
        process->kill();
        finish(false, {}, "The user service manager did not answer within its deadline.");
    });
    deadline->start(timeout);
    process->start(program, arguments);
}
QJsonObject parseProperties(const QByteArray& output) {
    QJsonObject result;
    for (const auto& line : output.split('\n')) {
        const auto split = line.indexOf('=');
        if (split > 0)
            result.insert(QString::fromLatin1(line.left(split)),
                          QString::fromLatin1(line.mid(split + 1)));
    }
    return result;
}
} // namespace

namespace {
void probeAsync(const QString& helper, const QStringList& extra, QObject* owner,
                std::function<void(bool, QString)> done) {
#ifdef Q_OS_LINUX
    if (geteuid() == 0 || linuxBootId().isEmpty()) {
        done(false, "The Linux executor requires a non-root daemon user and a boot identity.");
        return;
    }
    const auto task = QUuid::createUuid().toString(QUuid::Id128);
    const auto lease = QUuid::createUuid().toString(QUuid::Id128);
    auto arguments = properties();
    arguments << "--wait" << "--pipe" << "--property=RuntimeMaxSec=10s"
              << "--unit=" + linuxJobUnit(task, lease) << helper << "--linux-probe" << task << lease
              << extra;
    runAsync("/usr/bin/systemd-run", arguments, owner, 6000,
             [done = std::move(done)](bool ok, const QByteArray& output, const QString& error) {
                 const bool available =
                     ok &&
                     QJsonDocument::fromJson(output.trimmed()).object().value("available").toBool();
                 done(available,
                      available ? QString{}
                      : error.isEmpty()
                          ? QStringLiteral("Delegated cgroup v2, nsdelegate, isolated namespaces "
                                           "or user-unit cleanup are unavailable.")
                          : error);
             });
#else
    Q_UNUSED(helper)
    Q_UNUSED(extra)
    Q_UNUSED(owner)
    done(false, "Linux cgroup containment is unavailable on this platform.");
#endif
}
} // namespace

void linuxContainmentAvailableAsync(const QString& helper, QObject* owner,
                                    std::function<void(bool, QString)> done) {
    probeAsync(helper, {}, owner, std::move(done));
}

void linuxMountsAvailableAsync(const QString& helper, QObject* owner,
                               std::function<void(bool, QString)> done) {
    probeAsync(helper, {QStringLiteral("--mounts")}, owner, std::move(done));
}

void linuxUnitPropertiesAsync(const QString& unit, QObject* owner,
                              std::function<void(QJsonObject)> done) {
    if (!unit.startsWith("ketplus-job-") || !unit.endsWith(".service") || unit.contains('/')) {
        done({});
        return;
    }
    runAsync("/usr/bin/systemctl",
             {"--user", "show", unit,
              "--property=LoadState,ActiveState,SubState,MainPID,InvocationID,ControlGroup,"
              "Delegate,KillMode,TimeoutStopUSec"},
             owner, 4000, [done = std::move(done)](bool, const QByteArray& output, const QString&) {
                 done(parseProperties(output)); // Missing units can return a nonzero exit code.
             });
}

void linuxStartSupervisorAsync(const QString& helper, const QString& directory, const QString& task,
                               const QString& lease, QObject* owner, std::function<void()> done) {
    auto arguments = properties();
    arguments << "--unit=" + linuxJobUnit(task, lease) << helper << directory << task << lease;
    runAsync(
        "/usr/bin/systemd-run", arguments, owner, 4000,
        [done = std::move(done)](bool, const QByteArray&, const QString&) {
            done(); // Ambiguous replies always reconcile the durable unit; never replay launch.
        });
}

QJsonObject linuxCreateBoundary(const QString& task, const QString& lease, QString* error) {
    const auto unit = linuxJobUnit(task, lease);
    const auto service = linuxUnitProperties(unit, error);
    const auto relative = service.value("ControlGroup").toString();
    if (service.value("MainPID").toString().toLongLong() != QCoreApplication::applicationPid() ||
        service.value("Delegate") != "yes" || service.value("KillMode") != "control-group" ||
        service.value("TimeoutStopUSec") != "3s" ||
        service.value("InvocationID").toString().isEmpty() || !relative.startsWith('/') ||
        relative.contains("..") || !read("/proc/mounts").contains("nsdelegate")) {
        *error = "The supervisor must be the MainPID of its delegated, bounded user unit.";
        return {};
    }
    const auto path = "/sys/fs/cgroup" + relative + "/job";
    if (!QDir().mkdir(path)) {
        *error = "Unable to allocate a fresh job domain in the delegated service.";
        return {};
    }
    auto result = filesystemIdentity(path);
    if (result.isEmpty() || read(path + "/cgroup.type") != "domain" ||
        !QFile::exists(path + "/cgroup.kill") || !write(path + "/cgroup.kill", "1") ||
        read(path + "/cgroup.events").isEmpty()) {
        *error = "The delegated domain lacks cgroup.kill/events or domain semantics.";
        QDir().rmdir(path);
        return {};
    }
    result.insert("unit", unit);
    result.insert("invocation_id", service.value("InvocationID"));
    return result;
}

bool linuxBoundaryMatches(const QJsonObject& identity) {
    const auto current = filesystemIdentity(identity.value("path").toString());
    return !current.isEmpty() && current.value("boot_id") == identity.value("boot_id") &&
           current.value("inode") == identity.value("inode") &&
           current.value("device") == identity.value("device");
}
bool linuxBoundaryEmpty(const QJsonObject& identity) {
    return linuxBoundaryMatches(identity) &&
           read(identity.value("path").toString() + "/cgroup.events")
               .split('\n')
               .contains("populated 0");
}
LinuxPopulated linuxPopulatedOf(const QByteArray& events) {
    std::optional<LinuxPopulated> found;
    for (const QByteArray& line : events.split('\n')) {
        if (!line.startsWith("populated "))
            continue;
        const QByteArray value = line.mid(int(sizeof("populated ")) - 1);
        if (found || (value != "0" && value != "1"))
            return LinuxPopulated::Unknown;
        found = value == "0" ? LinuxPopulated::Empty : LinuxPopulated::Populated;
    }
    return found.value_or(LinuxPopulated::Unknown);
}
bool linuxAwaitEmpty(const std::function<std::optional<QByteArray>()>& readEvents,
                     const int deadlineMs, const int pollMs) {
    const QDeadlineTimer deadline(deadlineMs, Qt::PreciseTimer);
    for (;;) {
        const auto events = readEvents();
        const auto state = events ? linuxPopulatedOf(*events) : LinuxPopulated::Unknown;
        if (state != LinuxPopulated::Populated)
            return state == LinuxPopulated::Empty;
        const qint64 left = deadline.remainingTime();
        if (left <= 0)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<qint64>(pollMs, left)));
    }
}
bool linuxBoundaryEmptyWithin(const QJsonObject& identity, const int deadlineMs) {
    const QString path = identity.value("path").toString() + "/cgroup.events";
    return linuxAwaitEmpty(
        [&identity, &path]() -> std::optional<QByteArray> {
            if (!linuxBoundaryMatches(identity))
                return std::nullopt;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return std::nullopt;
            return file.read(16384).trimmed();
        },
        deadlineMs, linuxProbeEmptyPollMs);
}
bool linuxProbeSucceeded(const bool launched, const int waitStatus,
                         const std::function<bool()>& boundaryEmptied) {
    return launched && WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0 && boundaryEmptied();
}
bool linuxKillBoundary(const QJsonObject& identity) {
    return linuxBoundaryMatches(identity) &&
           write(identity.value("path").toString() + "/cgroup.kill", "1");
}
bool linuxPlaceLauncher(const QJsonObject& identity, qint64 pid) {
    return pid > 0 && linuxBoundaryMatches(identity) &&
           write(identity.value("path").toString() + "/cgroup.procs", QByteArray::number(pid));
}

bool linuxReceiptTrusted(const QString& task, const QString& lease, const QJsonObject& receipt,
                         const QJsonObject& service) {
    const auto unit = linuxJobUnit(task, lease);
    const auto boundary = receipt.value("linux_boundary").toObject();
    return !linuxBootId().isEmpty() && receipt.value("boot_id") == linuxBootId() &&
           receipt.value("systemd_unit") == unit && boundary.value("unit") == unit &&
           boundary.value("invocation_id") == service.value("InvocationID") &&
           service.value("MainPID").toString().toLongLong() > 0 &&
           service.value("MainPID").toString().toLongLong() ==
               receipt.value("supervisor_identity").toObject().value("pid").toInteger() &&
           boundary.value("path") ==
               "/sys/fs/cgroup" + service.value("ControlGroup").toString() + "/job" &&
           linuxBoundaryMatches(boundary);
}

QJsonObject linuxRecoverBoundary(const QString& task, const QString& lease,
                                 const QJsonObject& receipt, const QJsonObject& service) {
    const auto unit = linuxJobUnit(task, lease);
    const auto identity = receipt.value("linux_boundary").toObject();
    const auto oldBoot = receipt.value("boot_id").toString();
    const auto currentBoot = linuxBootId();
    if (currentBoot.isEmpty())
        return {{"clean", false}, {"reason", "linux_boot_identity_unavailable"}};
    if (!oldBoot.isEmpty() && oldBoot != currentBoot)
        return {{"clean", true}, {"reason", "host_rebooted"}};
    if (!identity.isEmpty() && linuxBoundaryMatches(identity)) {
        if (identity.value("unit") != unit ||
            identity.value("invocation_id") != service.value("InvocationID"))
            return {{"clean", false}, {"reason", "linux_unit_identity_changed"}};
        linuxKillBoundary(identity);
        return {{"clean", linuxBoundaryEmpty(identity)}, {"reason", "boundary_reclaimed"}};
    }
    if (!identity.isEmpty() && QFile::exists(identity.value("path").toString()))
        return {{"clean", false}, {"reason", "linux_subtree_identity_changed"}};
    if (service.value("LoadState") == "not-found" ||
        (service.value("ActiveState") == "inactive" &&
         service.value("ControlGroup").toString().isEmpty()))
        return {{"clean", true}, {"reason", "unit_reclaimed"}};
    // The deterministic unit belongs to this durable lease, never an arbitrary
    // process name/PID. A missing receipt cannot establish its subtree identity.
    return {{"clean", false}, {"reason", "linux_cleanup_pending"}};
}
} // namespace ketplus::daemon::executor
#endif // !Q_OS_WIN
