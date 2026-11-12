// ADR 0118: the pull request probe. `gh` is the tool the app's agents already use and sign in
// with; without it, or offline, every pull request is unknown and nothing is marked done.
#include "runtime/TeamPullRequests.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <algorithm>

namespace ketplus::daemon::team_pull_requests {
namespace {

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

// Nothing in `timedOut` when it answered, even with an error; set when it did not answer.
teams::PullRequestFact askOne(const QString& directory, const QString& program, const QString& url,
                              const int timeoutMs, const std::atomic_bool* cancelled,
                              bool* timedOut) {
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
    if (!process.waitForStarted(5000))
        return failure(QStringLiteral("gh could not be started."));
    // In short steps, so a daemon that stops does not wait for the network.
    QElapsedTimer clock;
    clock.start();
    bool finished = false;
    while (!(finished = process.waitForFinished(200)) && clock.elapsed() < timeoutMs &&
           !(cancelled && cancelled->load()))
        ;
    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
        *timedOut = true;
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
                                           const QString& program, const Limits limits,
                                           const std::atomic_bool* cancelled) {
    QHash<QString, teams::PullRequestFact> facts;
    const QString tool = program.isEmpty() ? findGh() : program;
    QElapsedTimer clock;
    clock.start();
    // Once the batch stops, the rest is left out: unknown, not an error, asked next time.
    bool stopped = false;
    for (const QString& url : urls) {
        if (facts.contains(url))
            continue;
        if (tool.isEmpty()) {
            facts.insert(url, failure(QStringLiteral("The GitHub command line tool (gh) is not "
                                                     "installed.")));
            continue;
        }
        const qint64 left = limits.batchMs - clock.elapsed();
        stopped = stopped || left <= 0 || (cancelled && cancelled->load());
        if (stopped)
            break;
        bool timedOut = false;
        facts.insert(url, askOne(directory, tool, url,
                                 static_cast<int>(std::min<qint64>(limits.requestMs, left)),
                                 cancelled, &timedOut));
        stopped = timedOut;
    }
    return facts;
}

QString commentUrl(const QByteArray& output) {
    const QStringList lines = QString::fromUtf8(output).split(u'\n', Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (line.startsWith(QStringLiteral("https://")) && line.contains(u'#') &&
            !line.contains(u' '))
            return line;
    }
    return {};
}

teams::ReviewComment comment(const QString& directory, const QString& url, const QString& body,
                             const QString& program, const int timeoutMs,
                             const std::atomic_bool* cancelled) {
    const auto failed = [](const QString& error) {
        return teams::ReviewComment{.url = {}, .error = error};
    };
    const QString tool = program.isEmpty() ? findGh() : program;
    if (tool.isEmpty())
        return failed(QStringLiteral("The GitHub command line tool (gh) is not installed."));
    QProcess process;
    if (!directory.isEmpty())
        process.setWorkingDirectory(directory);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GH_PROMPT_DISABLED"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GH_NO_UPDATE_NOTIFIER"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.start(tool, {QStringLiteral("pr"), QStringLiteral("comment"), url,
                         QStringLiteral("--body-file"), QStringLiteral("-")});
    if (!process.waitForStarted(5000))
        return failed(QStringLiteral("gh could not be started."));
    process.write(body.toUtf8());
    process.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    bool finished = false;
    while (!(finished = process.waitForFinished(200)) && clock.elapsed() < timeoutMs &&
           !(cancelled && cancelled->load()))
        ;
    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
        return failed(QStringLiteral("gh did not answer in time."));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString message =
            QString::fromUtf8(process.readAllStandardError()).trimmed().section(u'\n', 0, 0);
        return failed(message.isEmpty() ? QStringLiteral("gh failed.") : message.left(200));
    }
    const QString link = commentUrl(process.readAllStandardOutput());
    return link.isEmpty() ? failed(QStringLiteral("gh posted the comment but gave no link."))
                          : teams::ReviewComment{.url = link, .error = {}};
}

} // namespace ketplus::daemon::team_pull_requests
