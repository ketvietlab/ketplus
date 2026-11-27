#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "HeartbeatCatalogCallTiming.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QAbstractEventDispatcher>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDirListing>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

using namespace ketplus::daemon;

namespace {
using Access = CloudPrivacyTestAccess;
constexpr int catalogSize = 1996;
constexpr int pageLimit = 100;
constexpr auto fixtureMarker = ".heartbeat-worktree-catalog-fixture";
const QByteArray salt(32, 's');
const QString heartbeatPath = QStringLiteral("/v2/daemon/heartbeat");

qint64 cpuNs() {
    timespec time{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0)
        qFatal("Cannot read process CPU time");
    return qint64(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

struct Duration {
    qint64 wall{};
    qint64 cpu{};
    qsizetype calls{};
    void add(Duration other) {
        wall += other.wall;
        cpu += other.cpu;
        calls += other.calls;
    }
};

struct HeartbeatSample {
    Duration full;
    Duration catalog;
};

Duration catalogDuration() {
    const CatalogCallTiming timing = endCatalogCallTiming();
    return {timing.wallNs, timing.cpuNs, timing.calls};
}

template <typename Function> Duration measure(Function&& function) {
    QElapsedTimer clock;
    clock.start();
    const qint64 cpu = cpuNs();
    function();
    const qint64 consumed = cpuNs() - cpu;
    return {clock.nsecsElapsed(), consumed, 1};
}

double ms(qint64 ns) { return double(ns) / 1'000'000.0; }

qint64 percentile(QList<qint64> list, double fraction) {
    std::sort(list.begin(), list.end());
    return list.at(qsizetype(std::ceil(fraction * double(list.size()))) - 1);
}

void report(QTextStream& out, const QString& name, const QList<Duration>& series) {
    QList<qint64> cpu;
    QList<qint64> wall;
    for (const Duration& sample : series) {
        cpu.append(sample.cpu);
        wall.append(sample.wall);
    }
    out << name << ',' << series.size() << ',' << series.first().calls << ','
        << ms(percentile(cpu, 0.50)) << ',' << ms(percentile(cpu, 0.95)) << ','
        << ms(percentile(wall, 0.50)) << ',' << ms(percentile(wall, 0.95)) << Qt::endl;
}

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    if (!process.waitForFinished(30'000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        qFatal("Cannot create Git fixture: %s", process.readAllStandardError().constData());
}

void seed(const QDir& root, int repositories, int presentPercent, int sessions) {
    if (!root.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        qFatal("Fixture directory must be empty");
    LocalJournal journal(root.filePath("journal.sqlite"));
    if (!journal.isOpen())
        qFatal("Cannot create fixture journal");
    const int linked = catalogSize - repositories;
    const int liveLinked = qMax(0, catalogSize * presentPercent / 100 - repositories);
    QStringList mainPaths;
    for (int repo = 0; repo < repositories; ++repo) {
        const QString main = root.filePath(QStringLiteral("repo-%1").arg(repo, 3, 10, u'0'));
        if (!QDir().mkpath(main) || !journal.rememberWorktree(main))
            qFatal("Cannot register main checkout");
        mainPaths.append(main);
        git(main, {"init", "-q", "-b", "main"});
        git(main, {"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit",
                   "--allow-empty", "-qm", "Fixture"});
    }
    for (int index = 0; index < linked; ++index) {
        const QString& main = mainPaths.at(index % repositories);
        const QString checkout = main + QStringLiteral("-wt-%1").arg(index, 4, 10, u'0');
        // Spread missing rows across the sorted catalog, including early pages.
        const bool exists = (index + 1) * liveLinked / linked != index * liveLinked / linked;
        if (exists)
            git(main, {"worktree", "add", "-q", "--detach", "--no-checkout", checkout, "HEAD"});
        if (!journal.rememberWorktree(checkout) ||
            !journal.rememberWorktreeRepository(checkout, main))
            qFatal("Cannot register linked checkout");
    }
    // Several conversations per active checkout model a journal with many retained worktrees.
    // Limit the distinct active roots so unrelated first-heartbeat Git process startup is
    // tractable.
    for (int index = 0; index < sessions; ++index) {
        StoredSession session;
        session.id = QStringLiteral("fixture-%1").arg(index);
        session.worktreePath = mainPaths.at(index % qMin(4, repositories));
        session.adapter = "codex";
        session.state = "ready";
        session.generation = 1;
        if (!journal.upsertSession(session))
            qFatal("Cannot seed conversation");
    }
    QFile marker(root.filePath(fixtureMarker));
    if (!marker.open(QIODevice::WriteOnly))
        qFatal("Cannot mark the benchmark fixture");
}

enum Phase { Sql, CanonicalStat, Repository, Checkouts, Source, Contains, Timer, PhaseCount };
using Phases = std::array<Duration, PhaseCount>;

QJsonArray instrumentedSnapshots(LocalJournal& journal, Phases& phases, bool trace) {
    // Mirrors the control client's bounded list, retaining its evaluation order. The caller
    // compares the complete output with the production function on the same journal.
    const auto record = [&](Phase phase, auto&& operation) {
        if (trace)
            std::fprintf(stderr, "ketplus-phase:%d\n", int(phase));
        phases[phase].add(measure(operation));
        if (trace)
            std::fputs("ketplus-phase:end\n", stderr);
    };
    constexpr int pageSize = 128;
    QStringList paths;
    QSet<QString> included;
    QSet<QString> expandedRepositories;
    const auto contains = [&](const QSet<QString>& set, const QString& path) {
        bool found = false;
        record(Contains, [&] { found = set.contains(path); });
        return found;
    };
    const auto add = [&](const QString& path) {
        if (contains(included, path))
            return;
        QString canonical;
        record(CanonicalStat, [&] { canonical = QFileInfo(path).canonicalFilePath(); });
        if (canonical.isEmpty() || contains(included, canonical))
            return;
        bool isDir = false;
        record(CanonicalStat, [&] { isDir = QFileInfo(canonical).isDir(); });
        if (isDir) {
            included.insert(canonical);
            paths.append(canonical);
        }
    };
    const auto addJournalPages = [&](bool conversationsOnly) {
        QString after = QStringLiteral("");
        while (paths.size() < pageLimit) {
            QStringList page;
            record(Sql,
                   [&] { page = journal.worktreePathsAfter(after, pageSize, conversationsOnly); });
            for (const auto& path : page) {
                add(path);
                if (paths.size() == pageLimit)
                    return;
            }
            if (page.size() < pageSize)
                return;
            after = page.constLast();
        }
    };
    const auto expandRepositories = [&](QStringList seeds) {
        for (const auto& seed : seeds) {
            if (paths.size() == pageLimit)
                return;
            QString repository;
            record(Repository, [&] { repository = worktreeRepository(seed); });
            if (contains(expandedRepositories, repository))
                continue;
            expandedRepositories.insert(repository);
            QStringList checkouts;
            record(Checkouts, [&] { checkouts = repositoryCheckouts(repository); });
            for (const auto& checkout : checkouts) {
                add(checkout);
                if (paths.size() == pageLimit)
                    return;
            }
        }
    };
    addJournalPages(true);
    expandRepositories(paths);
    addJournalPages(false);
    expandRepositories(paths);
    QJsonArray result;
    for (const QString& path : std::as_const(paths)) {
        record(Source, [&] {
            QJsonObject entry = cloud_control::sourceContext(path, salt);
            entry.remove("branch");
            entry.insert("catalogRevision", 0);
            entry.insert("lifecycleRevision", 0);
            result.append(entry);
        });
    }
    // Quantify the read-clock overhead of this instrumentation, especially for contains().
    qsizetype calls = 0;
    for (const Duration& duration : phases)
        calls += duration.calls;
    for (qsizetype i = 0; i < calls; ++i)
        phases[Timer].add(measure([] {}));
    return result;
}

class ColdFiles {
  public:
    ColdFiles(const QDir& root, const QString& database) : database_(database) {
        using Flag = QDirListing::IteratorFlag;
        for (const auto& entry :
             QDirListing(root.path(), Flag::Recursive | Flag::IncludeHidden | Flag::FilesOnly)) {
            const QByteArray path = QFile::encodeName(entry.filePath());
            const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                qFatal("Cannot open fixture file for cache inspection");
            ::fsync(fd);
            ::close(fd);
            paths_.append(path);
        }
    }

    double evict() const {
        for (const auto& name : QSqlDatabase::connectionNames()) {
            auto database = QSqlDatabase::database(name, false);
            if (database.databaseName() == database_ &&
                !QSqlQuery(database).exec(QStringLiteral("PRAGMA shrink_memory")))
                qFatal("Cannot shrink the fixture's SQLite cache");
        }
        const auto pageSize = size_t(::sysconf(_SC_PAGESIZE));
        qsizetype pages = 0;
        qsizetype resident = 0;
        for (const auto& path : paths_) {
            const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                continue; // SQLite transient files can disappear between samples.
            if (::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0)
                qFatal("Cannot advise eviction of benchmark pages");
            const auto length = ::lseek(fd, 0, SEEK_END);
            if (length > 0) {
                const auto count = qsizetype((size_t(length) + pageSize - 1) / pageSize);
                QByteArray vector(count, Qt::Uninitialized);
                void* mapping = ::mmap(nullptr, size_t(length), PROT_READ, MAP_PRIVATE, fd, 0);
                if (mapping == MAP_FAILED ||
                    ::mincore(mapping, size_t(length),
                              reinterpret_cast<unsigned char*>(vector.data())) != 0)
                    qFatal("Cannot verify fixture page residency");
                ::munmap(mapping, size_t(length));
                pages += count;
                resident += std::count_if(vector.begin(), vector.end(),
                                          [](char value) { return (value & 1) != 0; });
            }
            ::close(fd);
        }
        return pages ? 100.0 * double(resident) / double(pages) : 0.0;
    }

  private:
    QString database_;
    QList<QByteArray> paths_;
};

qsizetype batches(const r03::Transport& transport) {
    return std::count_if(transport.requests.begin(), transport.requests.end(),
                         [](const auto& request) { return request.url().path() == heartbeatPath; });
}

void publish(CloudControlClient& client) {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                     &loop, [&] {
                         if (Access::heartbeatIdle(client))
                             loop.quit();
                     });
    Access::publishHeartbeat(client);
    if (!Access::heartbeatIdle(client)) {
        timeout.start(60'000);
        loop.exec();
    }
    if (!Access::heartbeatIdle(client))
        qFatal("Heartbeat fixture timed out");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {"fixture", "Create or reuse an isolated fixture directory.", "path"},
        {"mode", "all, prepare, snapshot, phases, heartbeat, idle, or noop.", "mode", "all"},
        {"repositories", "Number of Git repositories.", "n", "24"},
        {"present-percent", "Share of entries that exist on disk.", "percent", "75"},
        {"sessions", "Number of conversations driving actual heartbeat batches.", "n", "240"},
        {"iterations", "Warm samples per operation.", "n", "30"},
        {"cold-iterations", "Additional file-data-cold samples per operation (0 disables).", "n",
         "12"},
        {"idle-seconds", "Duration of the idle daemon-core observation.", "seconds", "48"},
        {"trace-phases", "Write phase markers to stderr for an external syscall trace."},
        {"no-warmup", "Skip the snapshot warmup, useful for differential syscall tracing."},
    });
    parser.process(app);
    const auto number = [&](const char* option, int minimum, int maximum) {
        bool ok = false;
        const int value = parser.value(option).toInt(&ok);
        if (!ok || value < minimum || value > maximum)
            qFatal("Invalid --%s", option);
        return value;
    };
    const QString mode = parser.value("mode");
    if (!QStringList{"all", "prepare", "snapshot", "phases", "heartbeat", "idle", "noop"}.contains(
            mode))
        qFatal("Invalid --mode");
    const int iterations = number("iterations", 1, 10000);
    const int coldIterations = number("cold-iterations", 0, 10000);
    const int repositories = number("repositories", 2, catalogSize - 1);
    const int sessions = number("sessions", 0, 10000);
    const int presentPercent = number("present-percent", 1, 100);
    if (catalogSize * presentPercent / 100 < repositories)
        qFatal("Fixture needs one present checkout per repository");
    const int idleSeconds = number("idle-seconds", 15, 3600);

    QTemporaryDir temporary;
    const QString fixture = parser.value("fixture");
    if (fixture.isEmpty() && !temporary.isValid())
        qFatal("Cannot create temporary fixture directory");
    QDir root(fixture.isEmpty() ? temporary.path() : fixture);
    if ((!fixture.isEmpty() && root.absolutePath() == QDir::rootPath()) ||
        (!root.exists() && !QDir().mkpath(root.absolutePath())))
        qFatal("Cannot create fixture directory");
    root = QDir(root.canonicalPath());
    const QString database = root.filePath("journal.sqlite");
    if (mode == "prepare" && fixture.isEmpty())
        qFatal("--mode prepare requires --fixture");
    if (!QFileInfo::exists(database)) {
        seed(root, repositories, presentPercent, sessions);
    } else {
        if (mode == "prepare")
            qFatal("Fixture already exists; choose an empty directory");
        if (!QFileInfo(root.filePath(fixtureMarker)).isFile() || QFileInfo(database).isSymLink())
            qFatal("Only an existing benchmark fixture can be reused");
    }

    LocalJournal journal(database);
    const auto catalog = journal.worktreePaths();
    if (!journal.isOpen() || catalog.size() != catalogSize)
        qFatal("Expected exactly 1,996 enrolled worktrees");
    if (mode == "prepare")
        return 0;
    const qsizetype sessionCount = journal.sessions().size();
    const qsizetype expectedBatches = qMax<qsizetype>(
        1, (sessionCount + maximumHeartbeatSessions - 1) / maximumHeartbeatSessions);
    CodexAdapter provider(root.filePath("nonexistent-provider"));
    SessionHost host(&journal, &provider);
    r03::Transport transport;
    transport.status = 200;
    transport.statusForPath.insert(heartbeatPath, 202);
    CloudControlClient client(&journal, &host);
    Access::configure(client, transport);
    Access::pathSalt(client, salt);
    const auto clearRequests = [&] {
        transport.requests.clear();
        transport.bodies.clear();
    };
    QTextStream out(stdout);
    out << "metric,samples,calls,cpu_median_ms,cpu_p95_ms,wall_median_ms,wall_p95_ms" << Qt::endl;
    // Retain the same setup and teardown for strace subtraction without sampling a snapshot.
    if (mode == "noop")
        return 0;
    const auto snapshot = [&] {
        const QJsonArray result = Access::worktreeSnapshots(client);
        if (result.isEmpty() || result.size() > pageLimit)
            qFatal("Unexpected snapshot size");
    };
    const auto heartbeat = [&] {
        clearRequests();
        beginCatalogCallTiming();
        const Duration time = measure([&] { publish(client); });
        const Duration catalog = catalogDuration();
        if (batches(transport) != expectedBatches || catalog.calls != 1)
            qFatal("Expected one catalog build across all heartbeat batches");
        return HeartbeatSample{time, catalog};
    };
    if (mode == "all" || mode == "snapshot") {
        if (!parser.isSet("no-warmup"))
            snapshot();
        QList<Duration> warmSnapshots;
        for (int i = 0; i < iterations; ++i)
            warmSnapshots.append(measure(snapshot));
        report(out, "snapshot-warm", warmSnapshots);
    }
    if (mode == "all" || mode == "heartbeat" || mode == "idle") {
        const auto first = heartbeat();
        out << "# batches_per_heartbeat=" << batches(transport)
            << ",first_heartbeat_cpu_ms=" << ms(first.full.cpu)
            << ",first_heartbeat_wall_ms=" << ms(first.full.wall) << Qt::endl;
    }
    if (mode == "all" || mode == "phases") {
        const QJsonArray expected = Access::worktreeSnapshots(client);
        std::array<QList<Duration>, PhaseCount> results;
        for (int i = 0; i < iterations; ++i) {
            Phases phases{};
            if (instrumentedSnapshots(journal, phases, parser.isSet("trace-phases")) != expected)
                qFatal("Phase instrumentation no longer matches the production snapshot");
            for (qsizetype phase = 0; phase < PhaseCount; ++phase)
                results[size_t(phase)].append(phases[size_t(phase)]);
        }
        const QStringList names{"sql",
                                "canonical-stat",
                                "worktree-repository",
                                "repository-checkouts",
                                "source-context",
                                "contains",
                                "timer-control"};
        for (qsizetype phase = 0; phase < names.size(); ++phase)
            report(out, names.at(phase), results[size_t(phase)]);
    }
    if (mode == "all" || mode == "heartbeat") {
        QList<Duration> samples;
        QList<Duration> catalogSamples;
        for (int i = 0; i < iterations; ++i) {
            const auto sample = heartbeat();
            samples.append(sample.full);
            catalogSamples.append(sample.catalog);
        }
        report(out, "heartbeat-warm", samples);
        report(out, "heartbeat-catalog-warm", catalogSamples);
    }
    if (coldIterations > 0 && (mode == "all" || mode == "snapshot" || mode == "heartbeat")) {
        const ColdFiles files(root, database);
        double largestResidentPercent = 0;
        for (const QString& operation : {QString("snapshot"), QString("heartbeat")}) {
            if (mode != "all" && mode != operation)
                continue;
            QList<Duration> samples;
            QList<Duration> catalogSamples;
            for (int i = 0; i < coldIterations; ++i) {
                largestResidentPercent = qMax(largestResidentPercent, files.evict());
                if (operation == "snapshot") {
                    samples.append(measure(snapshot));
                } else {
                    const auto sample = heartbeat();
                    samples.append(sample.full);
                    catalogSamples.append(sample.catalog);
                }
            }
            report(out, operation + "-file-data-cold", samples);
            if (operation == "heartbeat")
                report(out, "heartbeat-catalog-file-data-cold", catalogSamples);
        }
        out << "# max_resident_file_pages_after_eviction_percent=" << largestResidentPercent
            << Qt::endl;
    }
    if (mode == "all" || mode == "idle") {
        clearRequests();
        beginCatalogCallTiming();
        const Duration idle = measure([&] {
            QEventLoop loop;
            QTimer::singleShot(idleSeconds * 1000, Qt::PreciseTimer, &loop, &QEventLoop::quit);
            Access::startIdleTimers(client);
            loop.exec();
        });
        const Duration catalog = catalogDuration();
        const qsizetype batchCount = batches(transport);
        const qsizetype heartbeats = (batchCount + expectedBatches - 1) / expectedBatches;
        if (heartbeats == 0 || catalog.calls != heartbeats)
            qFatal("Expected one catalog build per idle heartbeat");
        out << "# idle_wall_seconds=" << double(idle.wall) / 1'000'000'000
            << ",idle_process_cpu_ms=" << ms(idle.cpu) << ",idle_batches=" << batchCount
            << ",catalog_cpu_ms=" << ms(catalog.cpu)
            << ",catalog_share_percent=" << 100.0 * double(catalog.cpu) / double(idle.cpu)
            << ",remaining_cpu_ms=" << ms(idle.cpu - catalog.cpu) << Qt::endl;
    }
}
