#include "snapshot/SealedGit.h"

#include <QDir>
#include <QProcess>
#include <QRegularExpression>

namespace ketplus::runner::snapshot {
namespace {

// A path that never exists, for HOME and XDG_CONFIG_HOME: git finds no ~/.gitconfig,
// ~/.config/git/ignore or ~/.config/git/attributes there.
QString sealedHome() {
    return QDir::rootPath() + QStringLiteral("nonexistent/ketplus-runner-sealed-home");
}

QString describe(const QStringList& arguments) {
    // Leaves out the -c pairs every call shares.
    QStringList shown;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("-c")) {
            ++i;
            continue;
        }
        shown << arguments.at(i);
    }
    return QStringLiteral("git ") + shown.join(QLatin1Char(' '));
}

} // namespace

QString minimumGitVersion() {
    return QStringLiteral("%1.%2").arg(minimumGitMajor).arg(minimumGitMinor);
}

bool parseGitVersion(const QString& text, int* major, int* minor) {
    static const QRegularExpression pattern(QStringLiteral("^git version (\\d+)\\.(\\d+)"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return false;
    *major = match.captured(1).toInt();
    *minor = match.captured(2).toInt();
    return true;
}

bool isObjectId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    return pattern.match(id).hasMatch();
}

SealedGit::SealedGit(GitTool tool) : m_tool(std::move(tool)) {}

QStringList SealedGit::sealedOptions() {
    // Repository configuration is the store's or mirror's own (written only by this library),
    // so these mainly pin defaults a future store config or git release could change.
    const QStringList values{
        QStringLiteral("core.hooksPath=/dev/null"),
        QStringLiteral("core.fsmonitor=false"),
        QStringLiteral("core.untrackedCache=false"),
        QStringLiteral("core.attributesFile=/dev/null"),
        QStringLiteral("core.excludesFile=/dev/null"),
        QStringLiteral("core.splitIndex=false"),
        QStringLiteral("core.protectHFS=true"),
        QStringLiteral("core.protectNTFS=true"),
        QStringLiteral("core.quotePath=false"),
        QStringLiteral("index.sparse=false"),
        QStringLiteral("submodule.recurse=false"),
        QStringLiteral("protocol.allow=never"),
        QStringLiteral("gc.auto=0"),
        QStringLiteral("maintenance.auto=false"),
        QStringLiteral("advice.addEmbeddedRepo=false"),
        QStringLiteral("init.defaultBranch=main"),
    };
    QStringList options;
    for (const QString& value : values)
        options << QStringLiteral("-c") << value;
    return options;
}

QProcessEnvironment SealedGit::environment() const {
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment environment;
    // Only what is needed to find and start git; nothing that selects a repository or config.
    for (const QString& name : {QStringLiteral("PATH"), QStringLiteral("TMPDIR"),
                                QStringLiteral("SYSTEMROOT"), QStringLiteral("DEVELOPER_DIR")}) {
        if (system.contains(name))
            environment.insert(name, system.value(name));
    }
    environment.insert(QStringLiteral("HOME"), sealedHome());
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), sealedHome());
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_SYSTEM"), QStringLiteral("/dev/null"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    environment.insert(QStringLiteral("GIT_ATTR_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_ASKPASS"), QStringLiteral("/bin/false"));
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_NO_REPLACE_OBJECTS"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_ALLOW_PROTOCOL"), QString());
    environment.insert(QStringLiteral("GIT_PROTOCOL_FROM_USER"), QStringLiteral("0"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return environment;
}

Result<QString> SealedGit::checkVersion() const {
    const auto output = start({QStringLiteral("--version")}, environment(), {});
    if (!output)
        return output.error();
    const QString text = QString::fromUtf8(output.value().out).trimmed();
    int major = 0;
    int minor = 0;
    if (!parseGitVersion(text, &major, &minor))
        return Error{ErrorCode::GitUnavailable, QStringLiteral("unrecognised git: %1").arg(text)};
    if (major < minimumGitMajor || (major == minimumGitMajor && minor < minimumGitMinor)) {
        return Error{
            ErrorCode::GitTooOld,
            QStringLiteral("%1 is older than the required %2").arg(text, minimumGitVersion())};
    }
    return text;
}

Result<GitOutput> SealedGit::run(const GitRun& run) const {
    if (run.gitDir.isEmpty())
        return Error{ErrorCode::InvalidArgument, QStringLiteral("git run without GIT_DIR")};
    QProcessEnvironment environment = this->environment();
    environment.insert(QStringLiteral("GIT_DIR"), run.gitDir);
    if (!run.workTree.isEmpty())
        environment.insert(QStringLiteral("GIT_WORK_TREE"), run.workTree);
    if (!run.indexFile.isEmpty())
        environment.insert(QStringLiteral("GIT_INDEX_FILE"), run.indexFile);
    for (const QString& pair : run.extraEnvironment) {
        const qsizetype equals = pair.indexOf(QLatin1Char('='));
        if (equals > 0)
            environment.insert(pair.left(equals), pair.mid(equals + 1));
    }
    return start(sealedOptions() + run.arguments, environment, run);
}

Result<GitOutput> SealedGit::initBare(const QString& path, const QString& objectFormat) const {
    QStringList arguments = sealedOptions();
    arguments << QStringLiteral("init") << QStringLiteral("--quiet") << QStringLiteral("--bare");
    if (!objectFormat.isEmpty())
        arguments << QStringLiteral("--object-format=") + objectFormat;
    arguments << QStringLiteral("--") << path;
    // An empty template directory: no sample hooks or description copied into the store.
    QProcessEnvironment environment = this->environment();
    environment.insert(QStringLiteral("GIT_TEMPLATE_DIR"), sealedHome());
    return start(arguments, environment, {});
}

Result<GitOutput> SealedGit::start(const QStringList& arguments,
                                   const QProcessEnvironment& environment, const GitRun& io) const {
    QProcess process;
    process.setProcessEnvironment(environment);
    // Inside the worktree when there is one, so git resolves pathspecs against it.
    process.setWorkingDirectory(io.workTree.isEmpty() ? QDir::rootPath() : io.workTree);
    if (!io.outputFile.isEmpty())
        process.setStandardOutputFile(io.outputFile, QIODevice::Truncate);
    if (!io.inputFile.isEmpty())
        process.setStandardInputFile(io.inputFile);
    else if (io.input.isEmpty())
        process.setStandardInputFile(QProcess::nullDevice());
    process.start(m_tool.program, arguments);
    if (!process.waitForStarted()) {
        return Error{
            ErrorCode::GitUnavailable,
            QStringLiteral("cannot start %1: %2").arg(m_tool.program, process.errorString())};
    }
    if (io.inputFile.isEmpty() && !io.input.isEmpty()) {
        process.write(io.input);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(m_tool.timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return Error{ErrorCode::GitFailed, QStringLiteral("%1 timed out").arg(describe(arguments))};
    }
    GitOutput output{process.readAllStandardOutput(), process.readAllStandardError()};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return Error{ErrorCode::GitFailed, QStringLiteral("%1 failed (%2): %3")
                                               .arg(describe(arguments))
                                               .arg(process.exitCode())
                                               .arg(QString::fromUtf8(output.err).trimmed())};
    }
    return output;
}

} // namespace ketplus::runner::snapshot
