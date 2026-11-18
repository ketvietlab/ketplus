#include "run/RunnerAssembly.h"

#include "Environment.h"
#include "delivery/JobDelivery.h"
#include "exec/NativeExecutor.h"
#ifdef Q_OS_LINUX
#include "exec/LinuxPolicy.h"
#include "exec/LinuxSystemHost.h"
#endif
#include "run/ClientRelayPort.h"
#include "run/ExecutorCommandHost.h"
#include "run/RunIdentity.h"
#include "run/RunnerLoop.h"
#include "run/WorkDirectory.h"
#include "snapshot/SnapshotStoreReceiver.h"
#include "state/AcceptedJobsFile.h"
#include "state/PinTrust.h"

#include <QUuid>

namespace ketplus::runner::app {
namespace {

// Accepted-jobs storage that cannot be read: nothing is accepted, so every spec is refused.
class UnreadableAcceptedJobs final : public session::AcceptedJobs {
  public:
    [[nodiscard]] bool contains(const QString&, qint64) const override { return false; }
    [[nodiscard]] bool add(const QString&, qint64) override { return false; }
};

[[maybe_unused]] QString runtimeDirectory(const Environment& environment) {
    return environment.variable("XDG_RUNTIME_DIR");
}

session::RunnerInfo runnerInfo(const RunnerIdentity& identity, const Environment& environment) {
    session::RunnerInfo info;
    info.runnerId = identity.credential.runnerId;
    info.runnerVersion = environment.runnerVersion;
    info.os = environment.platform.os;
    info.arch = environment.platform.arch;
    info.labels = delivery::effectiveLabels(identity.config.labels, environment.platform.arch);
    info.baseEnvironment = baseEnvironment(environment);
    return info;
}

} // namespace

QMap<QString, QString> baseEnvironment(const Environment& environment) {
    QMap<QString, QString> base;
    for (const char* name : {"PATH", "HOME", "LANG", "USER", "LOGNAME"}) {
        const QString value = environment.variable(name);
        if (!value.isEmpty())
            base.insert(QString::fromLatin1(name), value);
    }
    return base;
}

struct RunnerAssembly::Parts {
    Parts(PairingContext& context, const StateDirectory& directory, WorkDirectory work,
          std::unique_ptr<receiver::SnapshotStoreReceiver> store)
        : work(std::move(work)), store(std::move(store)), journal(directory), relay(context),
#ifdef Q_OS_LINUX
          host(runtimeDirectory(context.environment)),
          policy(host, exec::NativeExecutor::installedProgram(),
                 {directory.path(), this->work.path()}),
#endif
          executor(exec::NativeExecutor::installedProgram()), pins(context.state),
          trust(context.state) {
    }

    WorkDirectory work;
    std::unique_ptr<receiver::SnapshotStoreReceiver> store;
    std::optional<AcceptedJobsFile> acceptedFile;
    UnreadableAcceptedJobs unreadable;
    QString acceptedError;
    JobJournalFile journal;
    ClientRelayPort relay;
#ifdef Q_OS_LINUX
    // Ready once this machine can contain a job and hide the runner's own directories from it.
    exec::LinuxSystemHost host;
    exec::LinuxPolicy policy;
#else
    // Closed elsewhere: the runner reports draining and takes no job.
    exec::ClosedPolicy policy;
#endif
    exec::NativeExecutor executor;
    std::unique_ptr<ExecutorCommandHost> command;
    std::unique_ptr<IdentitySigner> signer;
    StatePins pins;
    PinTrust trust;
    std::unique_ptr<RunnerLoop> loop;
};

std::unique_ptr<RunnerAssembly> RunnerAssembly::create(PairingContext& context,
                                                       const RunnerIdentity& identity,
                                                       const StateDirectory& directory,
                                                       const QString& workDirOption, RunSeams seams,
                                                       ExitCode* failure) {
    const Environment& environment = context.environment;
    QString error;
    std::optional<WorkDirectory> work = WorkDirectory::open(
        WorkDirectory::resolve(workDirOption, identity.credential.runnerId, environment),
        directory.path(), &error);
    std::unique_ptr<receiver::SnapshotStoreReceiver> created;
    if (work)
        created = receiver::SnapshotStoreReceiver::create(directory, work->path(), {}, &error);
    if (!created) {
        environment.err(error);
        *failure = ExitCode::State;
        return nullptr;
    }
    auto parts = std::make_unique<Parts>(context, directory, std::move(*work), std::move(created));
    parts->acceptedFile = AcceptedJobsFile::load(directory, environment.now, &parts->acceptedError);
    if (parts->acceptedFile)
        parts->acceptedError.clear();
    else if (parts->acceptedError.isEmpty())
        parts->acceptedError = QStringLiteral("The record of accepted jobs cannot be read.");
    exec::ExecutorCommandRunner::Options options;
    options.directory = directory.path() + QStringLiteral("/executor");
#ifdef Q_OS_LINUX
    options.mounts = exec::ExecutorCommandRunner::Mounts{directory.path(), parts->work.path(),
                                                         runtimeDirectory(environment)};
#endif
    parts->command = std::make_unique<ExecutorCommandHost>(options, parts->executor, parts->policy);
    parts->signer = std::make_unique<IdentitySigner>(identity);

    receiver::SnapshotStoreReceiver& store = *parts->store;
    session::AcceptedJobs& accepted =
        parts->acceptedFile ? static_cast<session::AcceptedJobs&>(*parts->acceptedFile)
                            : parts->unreadable;
    parts->loop = std::make_unique<RunnerLoop>(RunnerDeps{
        environment,
        parts->relay,
        std::move(seams.socket),
        seams.transports,
        *parts->signer,
        parts->pins,
        parts->trust,
        parts->journal,
        accepted,
        parts->acceptedError,
        ReceiverHost{store,
                     [&store](auto ready, auto failed) {
                         store.answerTo(std::move(ready), std::move(failed));
                     },
                     [&store] { store.idle(); }},
        *parts->command,
        seams.timers,
        runnerInfo(identity, environment),
        identity.config.labels,
        [] { return QStringLiteral("acq_") + QUuid::createUuid().toString(QUuid::Id128); },
        std::move(seams.stopSignals),
    });
    return std::unique_ptr<RunnerAssembly>(new RunnerAssembly(std::move(parts)));
}

RunnerAssembly::RunnerAssembly(std::unique_ptr<Parts> parts) : m_parts(std::move(parts)) {}

RunnerAssembly::~RunnerAssembly() = default;

RunnerLoop& RunnerAssembly::loop() { return *m_parts->loop; }

const QString& RunnerAssembly::workDirectory() const { return m_parts->work.path(); }

} // namespace ketplus::runner::app
