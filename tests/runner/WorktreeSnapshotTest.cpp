#include "snapshot/WorktreeSnapshot.h"
#include "snapshot/SealedGit.h"
#include "snapshot/WorkspaceApply.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace ketplus::runner::snapshot;

namespace {

struct GitResult {
    bool ok = false;
    QByteArray out;
    QString err;
};

// Fixture git: a clean, private HOME so the developer's own configuration never shapes the
// fixtures, and an identity for commits.
QProcessEnvironment fixtureEnvironment(const QString& home) {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/.config"));
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"), QStringLiteral("Fixture"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("fixture@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"), QStringLiteral("Fixture"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("fixture@example.invalid"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return environment;
}

GitResult runGitIn(const QString& home, const QString& directory, const QStringList& arguments,
                   const QByteArray& input = {}, const QStringList& extraEnvironment = {}) {
    QProcess process;
    QProcessEnvironment environment = fixtureEnvironment(home);
    for (const QString& pair : extraEnvironment)
        environment.insert(pair.section(QLatin1Char('='), 0, 0), pair.section(QLatin1Char('='), 1));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-c"), QStringLiteral("init.defaultBranch=main")} +
                      arguments);
    process.waitForStarted();
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    process.waitForFinished(60'000);
    GitResult result;
    result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    result.out = process.readAllStandardOutput();
    result.err = QStringLiteral("git %1: %2")
                     .arg(arguments.join(QLatin1Char(' ')),
                          QString::fromUtf8(process.readAllStandardError()));
    return result;
}

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray readLink(const QString& path) {
    char buffer[4096];
    const ssize_t length = ::readlink(QFile::encodeName(path).constData(), buffer, sizeof buffer);
    return length < 0 ? QByteArray() : QByteArray(buffer, int(length));
}

bool makeSymlink(const QString& target, const QString& link) {
    QDir().mkpath(QFileInfo(link).path());
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) ==
           0;
}

bool setExecutable(const QString& path, bool executable) {
    const auto mode = executable ? 0755 : 0644;
    return ::chmod(QFile::encodeName(path).constData(), mode) == 0;
}

qint64 mtimeNs(const QString& path) {
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0)
        return -1;
#ifdef __APPLE__
    return qint64(info.st_mtimespec.tv_sec) * 1'000'000'000 + info.st_mtimespec.tv_nsec;
#else
    return qint64(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
#endif
}

// Every entry below `root` (hidden ones and the .git directory included) with its type,
// mode, size, mtime in nanoseconds and content digest or link target. Equal fingerprints
// mean nothing was created, removed, rewritten or even touched.
QMap<QString, QByteArray> fingerprint(const QString& root) {
    QMap<QString, QByteArray> entries;
    entries.insert(QStringLiteral("."), QByteArray::number(mtimeNs(root)));
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        struct stat info{};
        ::lstat(QFile::encodeName(path).constData(), &info);
        QByteArray value = QByteArray::number(info.st_mode, 8) + ' ' +
                           QByteArray::number(qint64(info.st_size)) + ' ' +
                           QByteArray::number(mtimeNs(path)) + ' ';
        if (S_ISLNK(info.st_mode))
            value += readLink(path);
        else if (S_ISREG(info.st_mode))
            value += QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex();
        entries.insert(QDir(root).relativeFilePath(path), value);
    }
    return entries;
}

struct Entry {
    QString mode;
    QByteArray content; // blob bytes, link target, or the gitlink's commit id
    bool operator==(const Entry& other) const {
        return mode == other.mode && content == other.content;
    }
};

class EnvironmentGuard {
  public:
    void set(const char* name, const QByteArray& value) {
        if (!m_saved.contains(name))
            m_saved.insert(name, qEnvironmentVariableIsSet(name)
                                     ? std::optional<QByteArray>(qgetenv(name))
                                     : std::nullopt);
        qputenv(name, value);
    }
    void unset(const char* name) {
        set(name, {});
        qunsetenv(name);
    }
    ~EnvironmentGuard() {
        for (auto it = m_saved.cbegin(); it != m_saved.cend(); ++it) {
            if (it.value())
                qputenv(it.key().constData(), *it.value());
            else
                qunsetenv(it.key().constData());
        }
    }

  private:
    QMap<QByteArray, std::optional<QByteArray>> m_saved;
};

} // namespace

#define GIT(var, ...)                                                                              \
    const GitResult var = runGitIn(m_home, __VA_ARGS__);                                           \
    QVERIFY2(var.ok, qPrintable(var.err))

#define EXPECT_OK(result) QVERIFY2((result).ok(), qPrintable((result).error().message))

class WorktreeSnapshotTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();

    void versionChecks();
    void unsafePathPredicate();
    void snapshotCapturesWorktreeState();
    void snapshotOfLinkedWorktree();
    void userRepositoryIsUnchanged();
    void secondPackHoldsOnlyNewObjects();
    void applyMatchesSnapshotTree();
    void applyRejectsBadPacksAndKeepsWorkspace();
    void secondApplyRewritesOnlyChangedFiles();
    void gitRunsSealed();
    void hostileTreeCannotLeaveWorkspace();
    void symlinksCannotRedirectWrites();
    void submodulesAndLfsAreReportedNotSent();
    void userExcludesFileIsHonoured();
    void filteredPathsAreNotRunNorSent();

  private:
    QString path(const QString& relative) const { return m_root->filePath(relative); }
    QString makeRepository(const QString& name);
    QMap<QString, Entry> treeOf(const QString& gitDir, const QString& commit);
    QMap<QString, Entry> workspaceOf(const QString& root, const QStringList& paths);
    QSet<QString> packObjects(const QString& packPath);
    QString hashOf(const QByteArray& content);
    QByteArray packContent(const QString& packPath);

    std::unique_ptr<QTemporaryDir> m_root;
    std::unique_ptr<EnvironmentGuard> m_environment;
    QString m_home;
};

void WorktreeSnapshotTest::initTestCase() {
    const SealedGit git;
    const auto version = git.checkVersion();
    if (!version)
        QSKIP(qPrintable(QStringLiteral("system git unusable: ") + version.error().message));
}

void WorktreeSnapshotTest::init() {
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    // Canonical, so /var vs /private/var on macOS never makes two spellings of one path.
    m_root = std::make_unique<QTemporaryDir>(QFileInfo(m_root->path()).canonicalFilePath() +
                                             QStringLiteral("/t-XXXXXX"));
    QVERIFY(m_root->isValid());
    m_home = path(QStringLiteral("home"));
    QDir().mkpath(m_home);
    // The library reads core.excludesFile from the caller's environment; the developer's own
    // configuration must not shape any test.
    m_environment = std::make_unique<EnvironmentGuard>();
    m_environment->set("HOME", m_home.toUtf8());
    m_environment->set("XDG_CONFIG_HOME", (m_home + QStringLiteral("/.config")).toUtf8());
    m_environment->unset("GIT_CONFIG_GLOBAL");
}

void WorktreeSnapshotTest::cleanup() {
    m_environment.reset();
    m_root.reset();
}

QString WorktreeSnapshotTest::makeRepository(const QString& name) {
    const QString repo = path(name);
    QDir().mkpath(repo);
    const auto init = runGitIn(m_home, repo, {QStringLiteral("init"), QStringLiteral("-q")});
    return init.ok ? repo : QString();
}

QMap<QString, Entry> WorktreeSnapshotTest::treeOf(const QString& gitDir, const QString& commit) {
    QMap<QString, Entry> entries;
    const QStringList dirArg{QStringLiteral("--git-dir=") + gitDir};
    // A snapshot store reads unchanged blobs from the repository it is bound to.
    QStringList alternate;
    if (const QByteArray objects =
            readFile(QDir(gitDir).filePath(QStringLiteral("ketplus-source-objects")));
        !objects.isEmpty())
        alternate << QStringLiteral("GIT_ALTERNATE_OBJECT_DIRECTORIES=") +
                         QString::fromUtf8(objects);
    const auto listing =
        runGitIn(m_home, m_home,
                 dirArg + QStringList{QStringLiteral("ls-tree"), QStringLiteral("-r"),
                                      QStringLiteral("-z"), commit});
    if (!listing.ok)
        return entries;
    for (const QByteArray& raw : listing.out.split('\0')) {
        if (raw.isEmpty())
            continue;
        const QByteArray meta = raw.left(raw.indexOf('\t'));
        const QString file = QString::fromUtf8(raw.mid(raw.indexOf('\t') + 1));
        const QList<QByteArray> parts = meta.split(' ');
        Entry entry{QString::fromLatin1(parts.at(0)), {}};
        if (entry.mode == QStringLiteral("160000")) {
            entry.content = parts.at(2);
        } else {
            entry.content =
                runGitIn(m_home, m_home,
                         dirArg + QStringList{QStringLiteral("cat-file"), QStringLiteral("blob"),
                                              QString::fromLatin1(parts.at(2))},
                         {}, alternate)
                    .out;
        }
        entries.insert(file, entry);
    }
    return entries;
}

QMap<QString, Entry> WorktreeSnapshotTest::workspaceOf(const QString& root,
                                                       const QStringList& paths) {
    QMap<QString, Entry> entries;
    for (const QString& relative : paths) {
        const QString file = QDir(root).filePath(relative);
        struct stat info{};
        if (::lstat(QFile::encodeName(file).constData(), &info) != 0)
            continue;
        if (S_ISLNK(info.st_mode))
            entries.insert(relative, {QStringLiteral("120000"), readLink(file)});
        else if (S_ISDIR(info.st_mode))
            entries.insert(relative, {QStringLiteral("dir"), {}});
        else
            entries.insert(relative, {(info.st_mode & S_IXUSR) ? QStringLiteral("100755")
                                                               : QStringLiteral("100644"),
                                      readFile(file)});
    }
    return entries;
}

QSet<QString> WorktreeSnapshotTest::packObjects(const QString& packPath) {
    QTemporaryDir scratch(path(QStringLiteral("packscan-XXXXXX")));
    runGitIn(m_home, scratch.path(),
             {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"),
              QStringLiteral("repo")});
    const QString copy = scratch.filePath(QStringLiteral("scan.pack"));
    QFile::copy(packPath, copy);
    runGitIn(m_home, scratch.path(),
             {QStringLiteral("--git-dir=repo"), QStringLiteral("index-pack"), copy});
    const auto shown = runGitIn(m_home, scratch.path(), {QStringLiteral("show-index")},
                                readFile(scratch.filePath(QStringLiteral("scan.idx"))));
    QSet<QString> objects;
    for (const QByteArray& line : shown.out.split('\n')) {
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() >= 2)
            objects.insert(QString::fromLatin1(parts.at(1)));
    }
    return objects;
}

// Every object in a pack, decompressed and concatenated, to search for leaked bytes.
QByteArray WorktreeSnapshotTest::packContent(const QString& packPath) {
    QTemporaryDir scratch(path(QStringLiteral("packdump-XXXXXX")));
    const QString repo = scratch.filePath(QStringLiteral("repo"));
    runGitIn(m_home, scratch.path(),
             {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), repo});
    const QStringList dir{QStringLiteral("--git-dir=") + repo};
    runGitIn(m_home, scratch.path(),
             dir + QStringList{QStringLiteral("index-pack"), QStringLiteral("--stdin")},
             readFile(packPath));
    return runGitIn(m_home, scratch.path(),
                    dir + QStringList{QStringLiteral("cat-file"),
                                      QStringLiteral("--batch-all-objects"),
                                      QStringLiteral("--batch")})
        .out;
}

QString WorktreeSnapshotTest::hashOf(const QByteArray& content) {
    return QString::fromLatin1(runGitIn(m_home, m_home,
                                        {QStringLiteral("hash-object"), QStringLiteral("--stdin")},
                                        content)
                                   .out.trimmed());
}

void WorktreeSnapshotTest::versionChecks() {
    int major = 0;
    int minor = 0;
    QVERIFY(parseGitVersion(QStringLiteral("git version 2.50.1 (Apple Git-155)"), &major, &minor));
    QCOMPARE(major, 2);
    QCOMPARE(minor, 50);
    QVERIFY(parseGitVersion(QStringLiteral("git version 2.32.0.windows.1\n"), &major, &minor));
    QCOMPARE(minor, 32);
    QVERIFY(!parseGitVersion(QStringLiteral("not git"), &major, &minor));
    QCOMPARE(minimumGitVersion(), QStringLiteral("2.32"));

    const QString old = path(QStringLiteral("old-git"));
    QVERIFY(writeFile(old, "#!/bin/sh\necho 'git version 2.31.9'\n"));
    QVERIFY(setExecutable(old, true));
    const QString repo = makeRepository(QStringLiteral("repo"));
    const auto tooOld = createSnapshot({repo, path(QStringLiteral("store"))}, GitTool{old});
    QVERIFY(!tooOld.ok());
    QCOMPARE(tooOld.error().code, ErrorCode::GitTooOld);
    const auto missing = createSnapshot({repo, path(QStringLiteral("store"))},
                                        GitTool{path(QStringLiteral("none"))});
    QCOMPARE(missing.error().code, ErrorCode::GitUnavailable);
    QVERIFY(!QFileInfo::exists(path(QStringLiteral("store"))));
}

void WorktreeSnapshotTest::unsafePathPredicate() {
    QVERIFY(isSafeSnapshotPath(QStringLiteral("src/main.cpp")));
    QVERIFY(isSafeSnapshotPath(QStringLiteral(".gitignore")));
    QVERIFY(isSafeSnapshotPath(QStringLiteral("a/.github/x")));
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../../x", "a//b", "./a", "a/.", ".git",
                            "a/.GIT/config", ".git./x", "x/.git /y", "git~1/config", "a\\b"})
        QVERIFY2(!isSafeSnapshotPath(QString::fromLatin1(bad)), bad);
}

void WorktreeSnapshotTest::snapshotCapturesWorktreeState() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(!repo.isEmpty());
    const auto at = [&](const char* name) {
        return QDir(repo).filePath(QString::fromLatin1(name));
    };
    QVERIFY(writeFile(at(".gitignore"), "build/\n*.log\n"));
    QVERIFY(writeFile(at("keep.txt"), "keep\n"));
    QVERIFY(writeFile(at("modify.txt"), "before\n"));
    QVERIFY(writeFile(at("staged.txt"), "base\n"));
    QVERIFY(writeFile(at("delete.txt"), "delete me\n"));
    QVERIFY(writeFile(at("removed.txt"), "git rm me\n"));
    QVERIFY(writeFile(at("old/name.txt"), "renamed content\n"));
    QVERIFY(writeFile(at("tool.sh"), "#!/bin/sh\n"));
    QVERIFY(writeFile(at("was-exec.sh"), "#!/bin/sh\n"));
    QVERIFY(setExecutable(at("was-exec.sh"), true));
    QVERIFY(makeSymlink(QStringLiteral("keep.txt"), at("link")));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});

    QVERIFY(writeFile(at("modify.txt"), "after\n"));
    QVERIFY(writeFile(at("staged.txt"), "staged\n"));
    GIT(stage, repo, {QStringLiteral("add"), QStringLiteral("staged.txt")});
    QVERIFY(writeFile(at("staged.txt"), "staged then edited\n"));
    QVERIFY(QFile::remove(at("delete.txt")));
    GIT(rm, repo, {QStringLiteral("rm"), QStringLiteral("-q"), QStringLiteral("removed.txt")});
    QVERIFY(QDir().mkpath(at("new")));
    GIT(mv, repo,
        {QStringLiteral("mv"), QStringLiteral("old/name.txt"), QStringLiteral("new/name.txt")});
    QVERIFY(setExecutable(at("tool.sh"), true));
    QVERIFY(setExecutable(at("was-exec.sh"), false));
    QVERIFY(QFile::remove(at("link")));
    QVERIFY(makeSymlink(QStringLiteral("modify.txt"), at("link")));
    QVERIFY(makeSymlink(QStringLiteral("../outside/target"), at("newlink")));
    QVERIFY(writeFile(at("untracked/new.txt"), "untracked\n"));
    QVERIFY(writeFile(at("build/out.o"), "object\n"));
    QVERIFY(writeFile(at("debug.log"), "log\n"));
    QVERIFY(writeFile(at(".git/info/exclude"), "secret.txt\n"));
    QVERIFY(writeFile(at("secret.txt"), "excluded\n"));

    const auto snapshot = createSnapshot({repo, path(QStringLiteral("store"))});
    EXPECT_OK(snapshot);
    QVERIFY(snapshot.value().submodulePaths.isEmpty());
    QVERIFY(snapshot.value().lfsPaths.isEmpty());

    const QMap<QString, Entry> expected{
        {QStringLiteral(".gitignore"), {QStringLiteral("100644"), "build/\n*.log\n"}},
        {QStringLiteral("keep.txt"), {QStringLiteral("100644"), "keep\n"}},
        {QStringLiteral("modify.txt"), {QStringLiteral("100644"), "after\n"}},
        {QStringLiteral("staged.txt"), {QStringLiteral("100644"), "staged then edited\n"}},
        {QStringLiteral("new/name.txt"), {QStringLiteral("100644"), "renamed content\n"}},
        {QStringLiteral("tool.sh"), {QStringLiteral("100755"), "#!/bin/sh\n"}},
        {QStringLiteral("was-exec.sh"), {QStringLiteral("100644"), "#!/bin/sh\n"}},
        {QStringLiteral("link"), {QStringLiteral("120000"), "modify.txt"}},
        {QStringLiteral("newlink"), {QStringLiteral("120000"), "../outside/target"}},
        {QStringLiteral("untracked/new.txt"), {QStringLiteral("100644"), "untracked\n"}},
    };
    const auto actual = treeOf(path(QStringLiteral("store")), snapshot.value().commit);
    QCOMPARE(actual.keys(), expected.keys());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        QCOMPARE(actual.value(it.key()).mode, it.value().mode);
        QCOMPARE(actual.value(it.key()).content, it.value().content);
    }

    // The same state snapshots to the same commit: identity and date are fixed.
    const auto again = createSnapshot({repo, path(QStringLiteral("store"))});
    EXPECT_OK(again);
    QCOMPARE(again.value().commit, snapshot.value().commit);
}

void WorktreeSnapshotTest::snapshotOfLinkedWorktree() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("shared.txt")), "main\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    const QString linked = path(QStringLiteral("linked"));
    GIT(worktree, repo,
        {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
         QStringLiteral("-b"), QStringLiteral("feature"), linked});
    QVERIFY(QFileInfo(QDir(linked).filePath(QStringLiteral(".git"))).isFile());

    QVERIFY(writeFile(QDir(linked).filePath(QStringLiteral("shared.txt")), "linked edit\n"));
    QVERIFY(writeFile(QDir(linked).filePath(QStringLiteral("only-linked.txt")), "new\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("only-main.txt")), "main only\n"));

    // One store serves every worktree of the repository.
    const QString store = path(QStringLiteral("store"));
    const auto fromLinked = createSnapshot({linked, store});
    EXPECT_OK(fromLinked);
    const auto linkedTree = treeOf(store, fromLinked.value().commit);
    QCOMPARE(linkedTree.keys(),
             (QList<QString>{QStringLiteral("only-linked.txt"), QStringLiteral("shared.txt")}));
    QCOMPARE(linkedTree.value(QStringLiteral("shared.txt")).content, QByteArray("linked edit\n"));

    const auto fromMain = createSnapshot({repo, store});
    EXPECT_OK(fromMain);
    const auto mainTree = treeOf(store, fromMain.value().commit);
    QCOMPARE(mainTree.keys(),
             (QList<QString>{QStringLiteral("only-main.txt"), QStringLiteral("shared.txt")}));
    QCOMPARE(mainTree.value(QStringLiteral("shared.txt")).content, QByteArray("main\n"));

    // A store bound to one repository refuses another.
    const QString other = makeRepository(QStringLiteral("other"));
    const auto mismatch = createSnapshot({other, store});
    QVERIFY(!mismatch.ok());
    QCOMPARE(mismatch.error().code, ErrorCode::StoreMismatch);
    // And it may never live inside the repository it reads.
    const auto insideRepo =
        createSnapshot({repo, QDir(repo).filePath(QStringLiteral(".git/ketplus-store"))});
    QCOMPARE(insideRepo.error().code, ErrorCode::InvalidArgument);
    QVERIFY(!QFileInfo::exists(QDir(repo).filePath(QStringLiteral(".git/ketplus-store"))));
}

void WorktreeSnapshotTest::userRepositoryIsUnchanged() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    for (int i = 0; i < 5; ++i)
        QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("f%1.txt").arg(i)), "v1\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    GIT(gc, repo, {QStringLiteral("gc"), QStringLiteral("-q")}); // packed refs and objects too
    const QString linked = path(QStringLiteral("linked"));
    GIT(worktree, repo,
        {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
         QStringLiteral("-b"), QStringLiteral("side"), linked});
    // Dirty both: staged, unstaged, untracked and a deletion, so git has everything to hash.
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("f0.txt")), "staged\n"));
    GIT(stage, repo, {QStringLiteral("add"), QStringLiteral("f0.txt")});
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("f1.txt")), "unstaged\n"));
    QVERIFY(QFile::remove(QDir(repo).filePath(QStringLiteral("f2.txt"))));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("new.txt")), "untracked\n"));
    QVERIFY(writeFile(QDir(linked).filePath(QStringLiteral("f3.txt")), "linked\n"));
    QVERIFY(writeFile(QDir(linked).filePath(QStringLiteral("extra.txt")), "linked new\n"));
    // Same content, new mtime: git rehashes it to a blob the repository already has, which is
    // where a writable alternate would "freshen" the repository's pack.
    QTest::qWait(1100);
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("f4.txt")), "v1\n"));
    // Let any timestamp git might write differ from the fixture's own.
    QTest::qWait(1100);

    const auto repoBefore = fingerprint(repo);
    const auto linkedBefore = fingerprint(linked);
    QVERIFY(repoBefore.contains(QStringLiteral(".git/index")));
    QVERIFY(repoBefore.contains(QStringLiteral(".git/HEAD")));
    QVERIFY(repoBefore.contains(QStringLiteral(".git/packed-refs")));

    const QString store = path(QStringLiteral("store"));
    const auto first = createSnapshot({repo, store});
    EXPECT_OK(first);
    const auto firstPack =
        writePack({store, first.value().commit, {}, path(QStringLiteral("1.pack"))});
    EXPECT_OK(firstPack);
    const auto second = createSnapshot({linked, store});
    EXPECT_OK(second);
    const auto delta = writePack(
        {store, second.value().commit, first.value().commit, path(QStringLiteral("2.pack"))});
    EXPECT_OK(delta);

    const auto repoAfter = fingerprint(repo);
    const auto linkedAfter = fingerprint(linked);
    for (auto it = repoBefore.cbegin(); it != repoBefore.cend(); ++it)
        QVERIFY2(repoAfter.value(it.key()) == it.value(), qPrintable(it.key()));
    QCOMPARE(repoAfter.keys(), repoBefore.keys());
    QCOMPARE(repoAfter, repoBefore);
    QCOMPARE(linkedAfter, linkedBefore);
}

void WorktreeSnapshotTest::secondPackHoldsOnlyNewObjects() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QMap<QString, QByteArray> files;
    for (int i = 0; i < 60; ++i) {
        const QString name = QStringLiteral("dir%1/file%2.txt").arg(i % 6).arg(i);
        files.insert(name, QByteArray("content of file ") + QByteArray::number(i) + '\n');
        QVERIFY(writeFile(QDir(repo).filePath(name), files.value(name)));
    }
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    const QString store = path(QStringLiteral("store"));
    const auto first = createSnapshot({repo, store});
    EXPECT_OK(first);
    const auto full =
        writePack({store, first.value().commit, {}, path(QStringLiteral("full.pack"))});
    EXPECT_OK(full);
    const auto fullObjects = packObjects(full.value().path);
    for (const QByteArray& content : files)
        QVERIFY(fullObjects.contains(hashOf(content)));

    const QString changed = QStringLiteral("dir3/file9.txt");
    QVERIFY(writeFile(QDir(repo).filePath(changed), "a small edit\n"));
    const auto second = createSnapshot({repo, store});
    EXPECT_OK(second);
    const auto delta = writePack(
        {store, second.value().commit, first.value().commit, path(QStringLiteral("delta.pack"))});
    EXPECT_OK(delta);

    const auto deltaObjects = packObjects(delta.value().path);
    QVERIFY(deltaObjects.contains(hashOf("a small edit\n")));
    QVERIFY(deltaObjects.contains(second.value().commit));
    QVERIFY(deltaObjects.contains(second.value().tree));
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (it.key() != changed)
            QVERIFY2(!deltaObjects.contains(hashOf(it.value())), qPrintable(it.key()));
    }
    // The new blob, the commit, the root tree and dir3's tree: nothing else.
    QCOMPARE(deltaObjects.size(), 4);

    QCOMPARE(delta.value().size, QFileInfo(delta.value().path).size());
    QVERIFY(delta.value().size < full.value().size);
    QCOMPARE(
        delta.value().sha256,
        QCryptographicHash::hash(readFile(delta.value().path), QCryptographicHash::Sha256).toHex());
    QCOMPARE(delta.value().baseCommit, first.value().commit);

    const auto missingBase = writePack({store, second.value().commit,
                                        QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
                                        path(QStringLiteral("x.pack"))});
    QCOMPARE(missingBase.error().code, ErrorCode::BaseMissing);
    const auto badId =
        writePack({store, QStringLiteral("--all"), {}, path(QStringLiteral("x.pack"))});
    QCOMPARE(badId.error().code, ErrorCode::InvalidArgument);
}

void WorktreeSnapshotTest::applyMatchesSnapshotTree() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "alpha\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("deep/er/b.txt")), "beta\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("run.sh")), "#!/bin/sh\necho hi\n"));
    QVERIFY(setExecutable(QDir(repo).filePath(QStringLiteral("run.sh")), true));
    QVERIFY(
        makeSymlink(QStringLiteral("deep/er/b.txt"), QDir(repo).filePath(QStringLiteral("to-b"))));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("empty.txt")), ""));
    const QString store = path(QStringLiteral("store"));
    const auto snapshot = createSnapshot({repo, store});
    EXPECT_OK(snapshot);
    const auto pack =
        writePack({store, snapshot.value().commit, {}, path(QStringLiteral("p.pack"))});
    EXPECT_OK(pack);

    const QString workspace = path(QStringLiteral("runner/ws"));
    const auto applied = applySnapshot({path(QStringLiteral("runner/mirror")),
                                        workspace,
                                        pack.value().path,
                                        pack.value().sha256,
                                        snapshot.value().commit,
                                        {}});
    EXPECT_OK(applied);
    QVERIFY(applied.value().previousCommit.isEmpty());

    const auto expected = treeOf(store, snapshot.value().commit);
    QCOMPARE(expected.size(), 5);
    const auto actual = workspaceOf(workspace, expected.keys());
    QCOMPARE(actual.keys(), expected.keys());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        QCOMPARE(actual.value(it.key()).mode, it.value().mode);
        QCOMPARE(actual.value(it.key()).content, it.value().content);
    }
    // Nothing else in the workspace: the mirror keeps its own git data elsewhere.
    QStringList present;
    QDirIterator it(workspace, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        present << QDir(workspace).relativeFilePath(it.next());
    present.sort();
    QCOMPARE(present, expected.keys());
}

void WorktreeSnapshotTest::applyRejectsBadPacksAndKeepsWorkspace() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "one\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("b.txt")), "two\n"));
    const QString store = path(QStringLiteral("store"));
    const auto first = createSnapshot({repo, store});
    EXPECT_OK(first);
    const auto firstPack =
        writePack({store, first.value().commit, {}, path(QStringLiteral("1.pack"))});
    EXPECT_OK(firstPack);
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "changed\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("c.txt")), "three\n"));
    const auto second = createSnapshot({repo, store});
    EXPECT_OK(second);
    const auto delta = writePack(
        {store, second.value().commit, first.value().commit, path(QStringLiteral("2.pack"))});
    EXPECT_OK(delta);

    const QString mirror = path(QStringLiteral("runner/mirror"));
    const QString workspace = path(QStringLiteral("runner/ws"));
    EXPECT_OK(applySnapshot({mirror,
                             workspace,
                             firstPack.value().path,
                             firstPack.value().sha256,
                             first.value().commit,
                             {}}));
    QVERIFY(writeFile(QDir(workspace).filePath(QStringLiteral("build/obj.o")), "obj\n"));
    const auto before = fingerprint(workspace);

    const auto expectRejected = [&](const ApplyRequest& request,
                                    std::initializer_list<ErrorCode> codes, const char* what) {
        const auto result = applySnapshot(request);
        QVERIFY2(!result.ok(), what);
        QVERIFY2(
            std::find(codes.begin(), codes.end(), result.error().code) != codes.end(),
            qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), result.error().message)));
        QVERIFY2(fingerprint(workspace) == before, what);
    };
    const QByteArray good = readFile(delta.value().path);

    expectRejected(
        {mirror, workspace, delta.value().path, QByteArray(64, '0'), second.value().commit, {}},
        {ErrorCode::PackDigestMismatch}, "wrong digest");

    ApplyLimits tight;
    tight.maxPackBytes = delta.value().size - 1;
    expectRejected(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, tight},
        {ErrorCode::PackTooLarge}, "pack over the limit");

    QByteArray corrupt = good;
    for (qsizetype i = 12; i < corrupt.size() - 20; i += 7)
        corrupt[i] = char(corrupt[i] ^ 0x5a);
    const QString corruptPath = path(QStringLiteral("corrupt.pack"));
    QVERIFY(writeFile(corruptPath, corrupt));
    expectRejected(
        {mirror, workspace, corruptPath, fileSha256(corruptPath), second.value().commit, {}},
        {ErrorCode::PackInvalid}, "corrupt pack");

    const QString truncatedPath = path(QStringLiteral("truncated.pack"));
    QVERIFY(writeFile(truncatedPath, good.left(good.size() / 2)));
    expectRejected(
        {mirror, workspace, truncatedPath, fileSha256(truncatedPath), second.value().commit, {}},
        {ErrorCode::PackInvalid}, "truncated pack");

    // A valid pack that does not carry the commit asked for.
    expectRejected({mirror,
                    workspace,
                    firstPack.value().path,
                    firstPack.value().sha256,
                    QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
                    {}},
                   {ErrorCode::SnapshotIncomplete}, "commit not in pack");

    ApplyLimits small;
    small.maxCheckoutBytes = 4;
    expectRejected(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, small},
        {ErrorCode::CheckoutTooLarge}, "tree over the checkout limit");

    // A delta pack into a mirror that never received its base: objects are missing.
    const QString freshWorkspace = path(QStringLiteral("runner/fresh"));
    QVERIFY(writeFile(QDir(freshWorkspace).filePath(QStringLiteral("existing.txt")), "keep\n"));
    const auto freshBefore = fingerprint(freshWorkspace);
    const auto missing = applySnapshot({path(QStringLiteral("runner/fresh-mirror")),
                                        freshWorkspace,
                                        delta.value().path,
                                        delta.value().sha256,
                                        second.value().commit,
                                        {}});
    QVERIFY(!missing.ok());
    QVERIFY2(missing.error().code == ErrorCode::PackInvalid ||
                 missing.error().code == ErrorCode::SnapshotIncomplete,
             qPrintable(missing.error().message));
    QCOMPARE(fingerprint(freshWorkspace), freshBefore);

    // After all of that, the real delta still applies.
    EXPECT_OK(applySnapshot(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, {}}));
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("a.txt"))), QByteArray("changed\n"));
}

void WorktreeSnapshotTest::secondApplyRewritesOnlyChangedFiles() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    const auto at = [&](const char* name) {
        return QDir(repo).filePath(QString::fromLatin1(name));
    };
    QVERIFY(writeFile(at(".gitignore"), "build/\n"));
    QVERIFY(writeFile(at("keep.txt"), "unchanged\n"));
    QVERIFY(writeFile(at("src/keep.cpp"), "int main() {}\n"));
    QVERIFY(writeFile(at("change.txt"), "old\n"));
    QVERIFY(writeFile(at("gone.txt"), "to delete\n"));
    QVERIFY(writeFile(at("olddir/nested.txt"), "dir goes away\n"));
    QVERIFY(writeFile(at("tool.sh"), "#!/bin/sh\n"));
    const QString store = path(QStringLiteral("store"));
    const auto first = createSnapshot({repo, store});
    EXPECT_OK(first);
    const auto firstPack =
        writePack({store, first.value().commit, {}, path(QStringLiteral("1.pack"))});
    EXPECT_OK(firstPack);

    const QString mirror = path(QStringLiteral("runner/mirror"));
    const QString workspace = path(QStringLiteral("runner/ws"));
    const auto ws = [&](const char* name) {
        return QDir(workspace).filePath(QString::fromLatin1(name));
    };
    EXPECT_OK(applySnapshot({mirror,
                             workspace,
                             firstPack.value().path,
                             firstPack.value().sha256,
                             first.value().commit,
                             {}}));
    // What a build leaves behind, and a fixed old mtime on files the next snapshot keeps.
    QVERIFY(writeFile(ws("build/CMakeCache.txt"), "cache\n"));
    QVERIFY(writeFile(ws("build/obj/keep.o"), "object\n"));
    QVERIFY(writeFile(ws("notes.txt"), "runner-local untracked\n"));
    const QDateTime old = QDateTime::fromSecsSinceEpoch(1'000'000'000);
    for (const char* name : {"keep.txt", "src/keep.cpp", "build/obj/keep.o"}) {
        QFile file(ws(name));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(old, QFileDevice::FileModificationTime));
    }
    const qint64 keepMtime = mtimeNs(ws("keep.txt"));
    const qint64 sourceMtime = mtimeNs(ws("src/keep.cpp"));
    const qint64 objectMtime = mtimeNs(ws("build/obj/keep.o"));

    QVERIFY(writeFile(at("change.txt"), "new\n"));
    QVERIFY(QFile::remove(at("gone.txt")));
    QVERIFY(QDir(at("olddir")).removeRecursively());
    QVERIFY(setExecutable(at("tool.sh"), true));
    QVERIFY(writeFile(at("added.txt"), "added\n"));
    const auto second = createSnapshot({repo, store});
    EXPECT_OK(second);
    const auto delta = writePack(
        {store, second.value().commit, first.value().commit, path(QStringLiteral("2.pack"))});
    EXPECT_OK(delta);
    const auto applied = applySnapshot(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, {}});
    EXPECT_OK(applied);
    QCOMPARE(applied.value().previousCommit, first.value().commit);

    QCOMPARE(mtimeNs(ws("keep.txt")), keepMtime);
    QCOMPARE(mtimeNs(ws("src/keep.cpp")), sourceMtime);
    QCOMPARE(readFile(ws("change.txt")), QByteArray("new\n"));
    QCOMPARE(readFile(ws("added.txt")), QByteArray("added\n"));
    QVERIFY(QFileInfo(ws("tool.sh")).isExecutable());
    QVERIFY(!QFileInfo::exists(ws("gone.txt")));
    QVERIFY(!QFileInfo::exists(ws("olddir")));
    QCOMPARE(readFile(ws("notes.txt")), QByteArray("runner-local untracked\n"));
    QCOMPARE(readFile(ws("build/CMakeCache.txt")), QByteArray("cache\n"));
    QCOMPARE(mtimeNs(ws("build/obj/keep.o")), objectMtime);

    // Applying the same snapshot again rewrites nothing at all.
    const auto settled = fingerprint(workspace);
    EXPECT_OK(applySnapshot(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, {}}));
    QCOMPARE(fingerprint(workspace), settled);
}

void WorktreeSnapshotTest::gitRunsSealed() {
    const QString markers = path(QStringLiteral("markers"));
    QDir().mkpath(markers);
    const QString touch = path(QStringLiteral("touch.sh"));
    // Every hostile program records that it ran, then behaves like a filter would.
    QVERIFY(writeFile(touch, QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() +
                                 "/$(basename \"$0\")-$$\"\ntr a-z A-Z\n"));
    QVERIFY(setExecutable(touch, true));
    const QString hooks = path(QStringLiteral("hostile-hooks"));
    for (const char* hook :
         {"pre-commit", "post-commit", "post-index-change", "reference-transaction",
          "post-checkout", "pre-auto-gc", "fsmonitor-watchman"}) {
        const QString file = QDir(hooks).filePath(QString::fromLatin1(hook));
        QVERIFY(writeFile(file, QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() + "/" + hook +
                                    "\"\n"));
        QVERIFY(setExecutable(file, true));
    }

    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "base\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});

    // Hostile repository: attributes select a filter its config defines, hooks everywhere,
    // and an fsmonitor command.
    QVERIFY(
        writeFile(QDir(repo).filePath(QStringLiteral(".gitattributes")), "*.dat filter=evil\n"));
    const QByteArray filter = QByteArray("[filter \"evil\"]\n\tclean = ") + touch.toUtf8() +
                              "\n\tsmudge = " + touch.toUtf8() + "\n\trequired = true\n";
    QFile config(QDir(repo).filePath(QStringLiteral(".git/config")));
    QVERIFY(config.open(QIODevice::Append));
    config.write(filter + "[core]\n\thooksPath = " + hooks.toUtf8() +
                 "\n\tfsmonitor = " + touch.toUtf8() + "\n");
    config.close();
    for (const char* hook : {"post-index-change", "pre-commit", "reference-transaction"}) {
        const QString file =
            QDir(repo).filePath(QStringLiteral(".git/hooks/") + QString::fromLatin1(hook));
        QVERIFY(writeFile(file, QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() + "/repo-" +
                                    hook + "\"\n"));
        QVERIFY(setExecutable(file, true));
    }

    // Hostile user and process environment, as a caller might inherit it.
    const QString hostileHome = path(QStringLiteral("hostile-home"));
    QVERIFY(writeFile(QDir(hostileHome).filePath(QStringLiteral(".gitignore-global")),
                      "globally-ignored.txt\n"));
    QVERIFY(writeFile(QDir(hostileHome).filePath(QStringLiteral(".config/git/attributes")),
                      "*.cfg filter=evil\n"));
    const QString globalConfig = QDir(hostileHome).filePath(QStringLiteral(".gitconfig"));
    QVERIFY(writeFile(globalConfig, filter + "[core]\n\thooksPath = " + hooks.toUtf8() +
                                        "\n\tfsmonitor = " + touch.toUtf8() +
                                        "\n\texcludesFile = " + hostileHome.toUtf8() +
                                        "/.gitignore-global\n\tautocrlf = true\n"));
    EnvironmentGuard guard;
    guard.set("HOME", hostileHome.toUtf8());
    guard.set("XDG_CONFIG_HOME", (hostileHome + QStringLiteral("/.config")).toUtf8());
    guard.set("GIT_CONFIG_GLOBAL", globalConfig.toUtf8());
    guard.set("GIT_CONFIG_PARAMETERS", "'core.hooksPath'='" + hooks.toUtf8() + "'");
    guard.set("GIT_CONFIG_COUNT", "1");
    guard.set("GIT_CONFIG_KEY_0", "filter.evil.clean");
    guard.set("GIT_CONFIG_VALUE_0", touch.toUtf8());
    guard.set("GIT_DIR", repo.toUtf8() + "/.git");
    guard.set("GIT_INDEX_FILE", repo.toUtf8() + "/.git/index");
    guard.set("GIT_WORK_TREE", repo.toUtf8());
    guard.set("GIT_OBJECT_DIRECTORY", repo.toUtf8() + "/.git/objects");

    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "hello\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("u.txt")), "world\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("globally-ignored.txt")), "hidden\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("data.dat")), "repo filtered\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("user.cfg")), "user filtered\n"));
    const auto repoBefore = fingerprint(repo);
    const QString store = path(QStringLiteral("store"));
    const auto snapshot = createSnapshot({repo, store});
    EXPECT_OK(snapshot);
    const auto pack =
        writePack({store, snapshot.value().commit, {}, path(QStringLiteral("p.pack"))});
    EXPECT_OK(pack);
    const QString workspace = path(QStringLiteral("runner/ws"));
    EXPECT_OK(applySnapshot({path(QStringLiteral("runner/mirror")),
                             workspace,
                             pack.value().path,
                             pack.value().sha256,
                             snapshot.value().commit,
                             {}}));
    QCOMPARE(fingerprint(repo), repoBefore);

    // Nothing ran, and contents are the raw bytes: no clean on the way in, no smudge out,
    // no autocrlf. The excludes file the configuration names is still read as data.
    QCOMPARE(QDir(markers).entryList(QDir::Files | QDir::Hidden), QStringList());
    const auto tree = treeOf(store, snapshot.value().commit);
    QCOMPARE(tree.value(QStringLiteral("a.txt")).content, QByteArray("hello\n"));
    QCOMPARE(tree.value(QStringLiteral("u.txt")).content, QByteArray("world\n"));
    QVERIFY(!tree.contains(QStringLiteral("globally-ignored.txt")));
    // Paths the repository's or the user's attributes filter: listed, never sent, never run.
    QStringList filtered = snapshot.value().filteredPaths;
    filtered.sort();
    QCOMPARE(filtered, (QStringList{QStringLiteral("data.dat"), QStringLiteral("user.cfg")}));
    QVERIFY(!tree.contains(QStringLiteral("data.dat")));
    QVERIFY(!tree.contains(QStringLiteral("user.cfg")));
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("a.txt"))), QByteArray("hello\n"));
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("u.txt"))), QByteArray("world\n"));
    // No sample hooks or other template content in the KetPlus-owned repositories either.
    QVERIFY(QDir(QDir(store).filePath(QStringLiteral("hooks"))).isEmpty());
}

void WorktreeSnapshotTest::hostileTreeCannotLeaveWorkspace() {
    // A desktop cannot produce these trees through createSnapshot; a forged pack can.
    const QString forge = path(QStringLiteral("forge"));
    GIT(init, m_home,
        {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), forge});
    const QStringList dir{QStringLiteral("--git-dir=") + forge};
    GIT(blob, m_home,
        dir + QStringList{QStringLiteral("hash-object"), QStringLiteral("-w"),
                          QStringLiteral("--stdin")},
        "escaped\n");
    const QByteArray blobId = QByteArray::fromHex(blob.out.trimmed());

    const QString workspace = path(QStringLiteral("runner/ws"));
    QVERIFY(writeFile(QDir(workspace).filePath(QStringLiteral("existing.txt")), "keep\n"));
    const auto before = fingerprint(workspace);
    const QStringList names{QStringLiteral("../escape.txt"), QStringLiteral(".git"),
                            QStringLiteral(".GIT"), QStringLiteral("/abs.txt"),
                            QStringLiteral("a/b")};
    int index = 0;
    for (const QString& name : names) {
        QByteArray tree;
        if (name == QStringLiteral(".git") || name == QStringLiteral(".GIT")) {
            QByteArray inner = "100644 config";
            inner += '\0';
            inner += blobId;
            GIT(innerTree, m_home,
                dir + QStringList{QStringLiteral("hash-object"), QStringLiteral("-t"),
                                  QStringLiteral("tree"), QStringLiteral("--literally"),
                                  QStringLiteral("-w"), QStringLiteral("--stdin")},
                inner);
            tree = "40000 " + name.toUtf8() + '\0' + QByteArray::fromHex(innerTree.out.trimmed());
        } else {
            tree = "100644 " + name.toUtf8() + '\0' + blobId;
        }
        GIT(treeId, m_home,
            dir + QStringList{QStringLiteral("hash-object"), QStringLiteral("-t"),
                              QStringLiteral("tree"), QStringLiteral("--literally"),
                              QStringLiteral("-w"), QStringLiteral("--stdin")},
            tree);
        GIT(commit, m_home,
            dir + QStringList{QStringLiteral("commit-tree"),
                              QString::fromLatin1(treeId.out.trimmed()), QStringLiteral("-m"),
                              QStringLiteral("forged")});
        const QString commitId = QString::fromLatin1(commit.out.trimmed());
        const QString packPath = path(QStringLiteral("forged-%1.pack").arg(index++));
        QProcess pack;
        pack.setProcessEnvironment(fixtureEnvironment(m_home));
        pack.setStandardOutputFile(packPath);
        pack.start(QStringLiteral("git"),
                   dir + QStringList{QStringLiteral("pack-objects"), QStringLiteral("--revs"),
                                     QStringLiteral("--stdout"), QStringLiteral("-q")});
        pack.write(commitId.toLatin1() + '\n');
        pack.closeWriteChannel();
        QVERIFY(pack.waitForFinished());
        QCOMPARE(pack.exitCode(), 0);

        const auto applied = applySnapshot({path(QStringLiteral("runner/mirror")),
                                            workspace,
                                            packPath,
                                            fileSha256(packPath),
                                            commitId,
                                            {}});
        QVERIFY2(!applied.ok(), qPrintable(name));
        QVERIFY2(applied.error().code == ErrorCode::PackInvalid ||
                     applied.error().code == ErrorCode::UnsafePath,
                 qPrintable(name + QStringLiteral(": ") + applied.error().message));
        QCOMPARE(fingerprint(workspace), before);
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("runner/escape.txt"))));
        QVERIFY(!QFileInfo::exists(QStringLiteral("/abs.txt")));
    }
}

void WorktreeSnapshotTest::symlinksCannotRedirectWrites() {
    const QString outside = path(QStringLiteral("outside"));
    QDir().mkpath(outside);
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("a.txt")), "a\n"));
    QVERIFY(makeSymlink(outside, QDir(repo).filePath(QStringLiteral("escape"))));
    const QString store = path(QStringLiteral("store"));
    const auto first = createSnapshot({repo, store});
    EXPECT_OK(first);
    const auto firstPack =
        writePack({store, first.value().commit, {}, path(QStringLiteral("1.pack"))});
    EXPECT_OK(firstPack);

    const QString mirror = path(QStringLiteral("runner/mirror"));
    const QString workspace = path(QStringLiteral("runner/ws"));
    // A link the runner workspace already had, outside any snapshot.
    QVERIFY(makeSymlink(outside, QDir(workspace).filePath(QStringLiteral("planted"))));
    EXPECT_OK(applySnapshot({mirror,
                             workspace,
                             firstPack.value().path,
                             firstPack.value().sha256,
                             first.value().commit,
                             {}}));
    QCOMPARE(readLink(QDir(workspace).filePath(QStringLiteral("escape"))), outside.toUtf8());

    // Next snapshot: both link names become directories holding files.
    QVERIFY(QFile::remove(QDir(repo).filePath(QStringLiteral("escape"))));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("escape/payload.txt")), "payload\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("planted/payload.txt")), "planted\n"));
    const auto second = createSnapshot({repo, store});
    EXPECT_OK(second);
    const auto delta = writePack(
        {store, second.value().commit, first.value().commit, path(QStringLiteral("2.pack"))});
    EXPECT_OK(delta);
    EXPECT_OK(applySnapshot(
        {mirror, workspace, delta.value().path, delta.value().sha256, second.value().commit, {}}));

    QCOMPARE(QDir(outside).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList());
    QVERIFY(!QFileInfo(QDir(workspace).filePath(QStringLiteral("escape"))).isSymLink());
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("escape/payload.txt"))),
             QByteArray("payload\n"));
    QVERIFY(!QFileInfo(QDir(workspace).filePath(QStringLiteral("planted"))).isSymLink());
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("planted/payload.txt"))),
             QByteArray("planted\n"));
}

void WorktreeSnapshotTest::submodulesAndLfsAreReportedNotSent() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    const QString sub = QDir(repo).filePath(QStringLiteral("vendor/lib"));
    QDir().mkpath(sub);
    GIT(subInit, sub, {QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(writeFile(QDir(sub).filePath(QStringLiteral("lib.c")), "submodule secret\n"));
    GIT(subAdd, sub, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(subCommit, sub, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("sub")});
    GIT(subHead, sub, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const QString subCommitId = QString::fromLatin1(subHead.out.trimmed());

    const QByteArray pointer = "version https://git-lfs.github.com/spec/v1\n"
                               "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24"
                               "d17e2393\nsize 22\n";
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral(".gitattributes")),
                      "*.bin filter=lfs diff=lfs merge=lfs -text\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("assets/model.bin")), pointer));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("main.c")), "int main;\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    // As git-lfs would leave the worktree: real content smudged in, plus a new untracked one.
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("assets/model.bin")),
                      "LARGE SMUDGED CONTENT\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("assets/new.bin")),
                      "LARGE UNTRACKED CONTENT\n"));

    const QString store = path(QStringLiteral("store"));
    const auto snapshot = createSnapshot({repo, store});
    EXPECT_OK(snapshot);
    QCOMPARE(snapshot.value().submodulePaths, QStringList{QStringLiteral("vendor/lib")});
    QStringList lfs = snapshot.value().lfsPaths;
    lfs.sort();
    QCOMPARE(lfs,
             (QStringList{QStringLiteral("assets/model.bin"), QStringLiteral("assets/new.bin")}));

    const auto tree = treeOf(store, snapshot.value().commit);
    QCOMPARE(tree.value(QStringLiteral("vendor/lib")).mode, QStringLiteral("160000"));
    QCOMPARE(tree.value(QStringLiteral("vendor/lib")).content, subCommitId.toLatin1());
    QCOMPARE(tree.value(QStringLiteral("assets/model.bin")).content, pointer);
    QVERIFY(!tree.contains(QStringLiteral("assets/new.bin")));

    const auto pack =
        writePack({store, snapshot.value().commit, {}, path(QStringLiteral("p.pack"))});
    EXPECT_OK(pack);
    const auto objects = packObjects(pack.value().path);
    QVERIFY(!objects.contains(subCommitId));
    QVERIFY(!objects.contains(hashOf("submodule secret\n")));
    QVERIFY(!objects.contains(hashOf("LARGE SMUDGED CONTENT\n")));
    QVERIFY(!objects.contains(hashOf("LARGE UNTRACKED CONTENT\n")));
    QVERIFY(objects.contains(hashOf(pointer)));

    const QString workspace = path(QStringLiteral("runner/ws"));
    EXPECT_OK(applySnapshot({path(QStringLiteral("runner/mirror")),
                             workspace,
                             pack.value().path,
                             pack.value().sha256,
                             snapshot.value().commit,
                             {}}));
    QCOMPARE(readFile(QDir(workspace).filePath(QStringLiteral("assets/model.bin"))), pointer);
    QVERIFY(QFileInfo(QDir(workspace).filePath(QStringLiteral("vendor/lib"))).isDir());
    QVERIFY(QDir(QDir(workspace).filePath(QStringLiteral("vendor/lib"))).isEmpty());
}

void WorktreeSnapshotTest::userExcludesFileIsHonoured() {
    const QString repo = makeRepository(QStringLiteral("repo"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("tracked.txt")), "tracked\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    for (const char* name : {".env", "key.pem", ".idea/workspace.xml", "local.secret",
                             "keep-negated.pem", "plain.txt"})
        QVERIFY(writeFile(QDir(repo).filePath(QString::fromLatin1(name)), "x\n"));
    const QString xdg = QDir(m_home).filePath(QStringLiteral(".config"));
    QVERIFY(writeFile(QDir(xdg).filePath(QStringLiteral("git/ignore")), ".env\n"));

    // Each case against the git the user runs, with the same configuration in place: the
    // snapshot's untracked files must be exactly what `git status` would show.
    const auto check = [&](const QString& label, const QStringList& gitEnvironment,
                           const UserGitSources& sources, const QStringList& expected) {
        const auto shown =
            runGitIn(m_home, repo,
                     {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--others"),
                      QStringLiteral("--exclude-standard")},
                     {}, gitEnvironment);
        QVERIFY2(shown.ok, qPrintable(shown.err));
        QStringList fromGit;
        for (const QByteArray& item : shown.out.split('\0'))
            if (!item.isEmpty())
                fromGit << QString::fromUtf8(item);
        fromGit.sort();
        QCOMPARE(fromGit, expected);

        const QString store = path(QStringLiteral("store-") + label);
        const auto snapshot = createSnapshot({repo, store, sources});
        QVERIFY2(snapshot.ok(), qPrintable(snapshot.error().message));
        QStringList untracked = treeOf(store, snapshot.value().commit).keys();
        untracked.removeAll(QStringLiteral("tracked.txt"));
        untracked.sort();
        QCOMPARE(untracked, expected);
    };

    // 1. No core.excludesFile anywhere: git's default $XDG_CONFIG_HOME/git/ignore.
    const QString globalConfig = QDir(m_home).filePath(QStringLiteral(".gitconfig"));
    check(QStringLiteral("default"),
          {QStringLiteral("GIT_CONFIG_GLOBAL=") + globalConfig,
           QStringLiteral("XDG_CONFIG_HOME=") + xdg},
          {m_home, xdg, {}},
          {QStringLiteral(".idea/workspace.xml"), QStringLiteral("keep-negated.pem"),
           QStringLiteral("key.pem"), QStringLiteral("local.secret"), QStringLiteral("plain.txt")});

    // 2. ~/.gitconfig names a file under ~; it replaces the default, so .env shows again.
    QVERIFY(writeFile(QDir(m_home).filePath(QStringLiteral("custom-ignore")),
                      "*.pem\n!keep-negated.pem\n.idea/\n"));
    QVERIFY(writeFile(globalConfig, "[core]\n\texcludesFile = ~/custom-ignore\n"));
    check(QStringLiteral("home"),
          {QStringLiteral("GIT_CONFIG_GLOBAL=") + globalConfig,
           QStringLiteral("XDG_CONFIG_HOME=") + xdg},
          {m_home, xdg, {}},
          {QStringLiteral(".env"), QStringLiteral("keep-negated.pem"),
           QStringLiteral("local.secret"), QStringLiteral("plain.txt")});

    // 3. The repository's own config wins over the global one, with a worktree-relative
    //    path; info/exclude still applies on top.
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("repo-ignore")), "local.secret\n"));
    GIT(local, repo,
        {QStringLiteral("config"), QStringLiteral("core.excludesFile"),
         QStringLiteral("repo-ignore")});
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral(".git/info/exclude")), "plain.txt\n"));
    check(QStringLiteral("repo"),
          {QStringLiteral("GIT_CONFIG_GLOBAL=") + globalConfig,
           QStringLiteral("XDG_CONFIG_HOME=") + xdg},
          {m_home, xdg, {}},
          {QStringLiteral(".env"), QStringLiteral(".idea/workspace.xml"),
           QStringLiteral("keep-negated.pem"), QStringLiteral("key.pem"),
           QStringLiteral("repo-ignore")});
    GIT(unsetLocal, repo,
        {QStringLiteral("config"), QStringLiteral("--unset"), QStringLiteral("core.excludesFile")});
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral(".git/info/exclude")), ""));

    // 4. GIT_CONFIG_GLOBAL in the caller's environment replaces ~/.gitconfig, as for git.
    const QString alternative = path(QStringLiteral("alt.gitconfig"));
    QVERIFY(writeFile(path(QStringLiteral("alt-ignore")), "plain.txt\n"));
    QVERIFY(writeFile(alternative, QByteArray("[core]\n\texcludesFile = ") +
                                       path(QStringLiteral("alt-ignore")).toUtf8() + "\n"));
    // 5. An includeIf section matching this repository names the excludes file, with `~`.
    QVERIFY(writeFile(QDir(m_home).filePath(QStringLiteral("included-ignore")), "key.pem\n"));
    QVERIFY(writeFile(QDir(m_home).filePath(QStringLiteral("included.inc")),
                      "[core]\n\texcludesFile = ~/included-ignore\n"));
    QVERIFY(writeFile(globalConfig, QByteArray("[core]\n\texcludesFile = ~/custom-ignore\n"
                                               "[includeIf \"gitdir:") +
                                        repo.toUtf8() + "/\"]\n\tpath = " + m_home.toUtf8() +
                                        "/included.inc\n"));
    const auto repoBefore = fingerprint(repo);
    check(QStringLiteral("include-if"),
          {QStringLiteral("GIT_CONFIG_GLOBAL=") + globalConfig,
           QStringLiteral("XDG_CONFIG_HOME=") + xdg},
          {m_home, xdg, {}},
          {QStringLiteral(".env"), QStringLiteral(".idea/workspace.xml"),
           QStringLiteral("keep-negated.pem"), QStringLiteral("local.secret"),
           QStringLiteral("plain.txt"), QStringLiteral("repo-ignore")});
    // The configuration query is the one git command run on the user's repository.
    QCOMPARE(fingerprint(repo), repoBefore);

    m_environment->set("GIT_CONFIG_GLOBAL", alternative.toUtf8());
    check(QStringLiteral("environment"),
          {QStringLiteral("GIT_CONFIG_GLOBAL=") + alternative,
           QStringLiteral("XDG_CONFIG_HOME=") + xdg},
          userGitSourcesFromEnvironment(),
          {QStringLiteral(".env"), QStringLiteral(".idea/workspace.xml"),
           QStringLiteral("keep-negated.pem"), QStringLiteral("key.pem"),
           QStringLiteral("local.secret"), QStringLiteral("repo-ignore")});
}

void WorktreeSnapshotTest::filteredPathsAreNotRunNorSent() {
    const QString markers = path(QStringLiteral("markers"));
    QDir().mkpath(markers);
    // An "encrypting" filter: rot13, recording each run.
    const QString filter = path(QStringLiteral("rot13.sh"));
    QVERIFY(writeFile(filter, QByteArray("#!/bin/sh\ntouch \"") + markers.toUtf8() +
                                  "/run-$$\"\ntr A-Za-z N-ZA-Mn-za-m\n"));
    QVERIFY(setExecutable(filter, true));
    const QString repo = makeRepository(QStringLiteral("repo"));
    GIT(clean, repo, {QStringLiteral("config"), QStringLiteral("filter.crypt.clean"), filter});
    GIT(smudge, repo, {QStringLiteral("config"), QStringLiteral("filter.crypt.smudge"), filter});
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral(".gitattributes")),
                      "*.secret filter=crypt\n*.txt -filter\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("app.secret")), "PLAINTEXT-ONE\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("notes.txt")), "notes\n"));
    GIT(add, repo, {QStringLiteral("add"), QStringLiteral("-A")});
    GIT(commit, repo, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
    GIT(stored, repo,
        {QStringLiteral("cat-file"), QStringLiteral("blob"), QStringLiteral("HEAD:app.secret")});
    QCOMPARE(stored.out, QByteArray("CYNVAGRKG-BAR\n")); // what the repository holds
    QVERIFY(QDir(markers).removeRecursively());
    QDir().mkpath(markers);

    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("app.secret")), "PLAINTEXT-TWO\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("new.secret")), "PLAINTEXT-THREE\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("notes.txt")), "notes edited\n"));
    // A filter set only by the user's own attributes file counts too.
    const QString xdg = QDir(m_home).filePath(QStringLiteral(".config"));
    QVERIFY(
        writeFile(QDir(xdg).filePath(QStringLiteral("git/attributes")), "*.key filter=crypt\n"));
    QVERIFY(writeFile(QDir(repo).filePath(QStringLiteral("id.key")), "PLAINTEXT-FOUR\n"));

    const QString store = path(QStringLiteral("store"));
    const auto snapshot = createSnapshot({repo, store, UserGitSources{m_home, xdg, {}}});
    EXPECT_OK(snapshot);
    QStringList filtered = snapshot.value().filteredPaths;
    filtered.sort();
    QCOMPARE(filtered, (QStringList{QStringLiteral("app.secret"), QStringLiteral("id.key"),
                                    QStringLiteral("new.secret")}));
    QVERIFY(snapshot.value().lfsPaths.isEmpty());

    const auto tree = treeOf(store, snapshot.value().commit);
    QCOMPARE(tree.value(QStringLiteral("app.secret")).content, QByteArray("CYNVAGRKG-BAR\n"));
    QVERIFY(!tree.contains(QStringLiteral("new.secret")));
    QVERIFY(!tree.contains(QStringLiteral("id.key")));
    QCOMPARE(tree.value(QStringLiteral("notes.txt")).content, QByteArray("notes edited\n"));

    const auto pack =
        writePack({store, snapshot.value().commit, {}, path(QStringLiteral("p.pack"))});
    EXPECT_OK(pack);
    const QByteArray content = packContent(pack.value().path);
    QVERIFY(content.contains("notes edited")); // the dump really holds the pack's objects
    QVERIFY(!content.contains("PLAINTEXT"));
    QCOMPARE(QDir(markers).entryList(QDir::Files | QDir::Hidden), QStringList());
}

QTEST_GUILESS_MAIN(WorktreeSnapshotTest)
#include "WorktreeSnapshotTest.moc"
