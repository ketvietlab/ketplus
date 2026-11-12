// ADR 0118: the pull request probe. `gh` is the tool the app's agents already use and sign in
// with; without it, or offline, every pull request is unknown and nothing is marked done.
#include "runtime/TeamPullRequests.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace ketplus::daemon::team_pull_requests {
namespace {

constexpr int timeoutMs = 20000;

teams::PullRequestFact failure(const QString& error) {
    return {.state = {}, .mergeCommit = {}, .error = error};
}

QString findGh() {
    QString found = QStandardPaths::findExecutable(QStringLiteral("gh"));
    // A daemon started by the desktop app may not have the shell's path.
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(
            QStringLiteral("gh"), {QStringLiteral("/opt/homebrew/bin"),
                                   QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
    return found;
}

teams::PullRequestFact askOne(const QString& directory, const QString& program,
                              const QString& url) {
    QProcess process;
    if (!directory.isEmpty())
        process.setWorkingDirectory(directory);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GH_PROMPT_DISABLED"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GH_NO_UPDATE_NOTIFIER"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.start(program, {QStringLiteral("pr"), QStringLiteral("view"), url,
                            QStringLiteral("--json"), QStringLiteral("state,mergeCommit")});
    if (!process.waitForStarted(timeoutMs))
        return failure(QStringLiteral("gh could not be started."));
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return failure(QStringLiteral("gh did not answer in time."));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString message =
            QString::fromUtf8(process.readAllStandardError()).trimmed().section(u'\n', 0, 0);
        return failure(message.isEmpty() ? QStringLiteral("gh failed.") : message.left(200));
    }
    return parse(process.readAllStandardOutput());
}

} // namespace

teams::PullRequestFact parse(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return failure(QStringLiteral("gh gave an answer that could not be read."));
    const QJsonObject object = document.object();
    const QString state = object.value(QStringLiteral("state")).toString().toLower();
    if (state != QStringLiteral("open") && state != QStringLiteral("merged") &&
        state != QStringLiteral("closed"))
        return failure(QStringLiteral("gh gave no known state."));
    // A squash or rebase merge has a merge commit too: the one on the base branch.
    const QString commit = object.value(QStringLiteral("mergeCommit"))
                               .toObject()
                               .value(QStringLiteral("oid"))
                               .toString();
    return {.state = state,
            .mergeCommit = state == QStringLiteral("merged") ? commit : QString{},
            .error = {}};
}

QHash<QString, teams::PullRequestFact> ask(const QString& directory, const QStringList& urls,
                                           const QString& program) {
    QHash<QString, teams::PullRequestFact> facts;
    const QString tool = program.isEmpty() ? findGh() : program;
    for (const QString& url : urls) {
        if (facts.contains(url))
            continue;
        facts.insert(url, tool.isEmpty()
                              ? failure(QStringLiteral("The GitHub command line tool (gh) is "
                                                       "not installed."))
                              : askOne(directory, tool, url));
    }
    return facts;
}

} // namespace ketplus::daemon::team_pull_requests
