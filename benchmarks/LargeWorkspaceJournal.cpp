// Writes a daemon journal holding a very large, synthetic workspace: thousands of
// conversations spread over many Git repositories. It exercises what grows with a
// long-lived install (the sidebar, the session list, restore) rather than one long
// transcript, which SyntheticTranscriptJournal covers. No real data, no provider.
//
// Usage: ketplus_large_workspace_journal <outdir> [conversations=3000]
//        [repositories=150] [turns=3] [archivedPercent=10]
// Writes <outdir>/journal.sqlite3 and initialises <outdir>/repos/repo-NNN as Git
// repositories, so the desktop groups every conversation under its repository.

#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>

#include <cstdio>

using namespace ketplus::daemon;

namespace {

quint32 gSeed = 20261117;
quint32 next() {
    gSeed = gSeed * 1664525u + 1013904223u;
    return gSeed >> 8;
}

QString words(const int count) {
    static const QStringList vocabulary{
        "sidebar", "restore", "journal",  "window",  "render", "cache",  "conversation",
        "layout",  "measure", "scroll",   "refresh", "repo",   "team",   "schedule",
        "parser",  "daemon",  "provider", "review",  "branch", "commit", "fixture"};
    QStringList result;
    for (int index = 0; index < count; ++index)
        result.append(vocabulary.at(int(next() % vocabulary.size())));
    return result.join(' ');
}

bool initialiseRepository(const QString& path) {
    QDir().mkpath(path);
    QProcess git;
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q"), path});
    return git.waitForFinished(30000) && git.exitCode() == 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr,
                     "usage: %s outdir [conversations] [repositories] [turns] "
                     "[archivedPercent]\n",
                     argv[0]);
        return 2;
    }
    const QDir out(args.at(1));
    const int conversations = args.size() > 2 ? args.at(2).toInt() : 3000;
    const int repositories = std::max(1, args.size() > 3 ? args.at(3).toInt() : 150);
    const int turns = args.size() > 4 ? args.at(4).toInt() : 3;
    const int archivedPercent = args.size() > 5 ? args.at(5).toInt() : 10;
    out.mkpath(QStringLiteral("repos"));
    QFile::remove(out.filePath("journal.sqlite3"));

    QStringList roots;
    for (int index = 0; index < repositories; ++index) {
        const QString path =
            out.filePath(QStringLiteral("repos/repo-%1").arg(index, 3, 10, QLatin1Char('0')));
        if (!QDir(path).exists(QStringLiteral(".git")) && !initialiseRepository(path)) {
            std::fprintf(stderr, "git init failed: %s\n", qPrintable(path));
            return 3;
        }
        roots.append(QFileInfo(path).canonicalFilePath());
    }

    LocalJournal journal(out.filePath("journal.sqlite3"));
    if (!journal.isOpen()) {
        std::fprintf(stderr, "journal: %s\n", qPrintable(journal.errorString()));
        return 4;
    }
    const QDateTime start = QDateTime::currentDateTimeUtc().addDays(-180);
    int archived = 0;
    int events = 0;
    for (int index = 0; index < conversations; ++index) {
        StoredSession session;
        session.id = QStringLiteral("large-%1").arg(index, 6, 10, QLatin1Char('0'));
        // A few busy repositories hold most conversations, as real installs do.
        const int skewed = int((next() % 1000) * (next() % 1000) / 1000) % repositories;
        session.worktreePath = roots.at(skewed);
        session.adapter = next() % 3 == 0 ? QStringLiteral("claude") : QStringLiteral("codex");
        session.threadId = session.id + QStringLiteral("-thread");
        session.state = QStringLiteral("ready");
        session.title = words(3 + int(next() % 6));
        session.lastTurnStartedAt =
            start.addSecs(qint64(index) * 5 + qint64(next() % 600)).toString(Qt::ISODateWithMs);
        if (!journal.upsertSession(session)) {
            std::fprintf(stderr, "upsert failed: %s\n", qPrintable(journal.errorString()));
            return 5;
        }
        for (int turn = 1; turn <= turns; ++turn) {
            QString error;
            const auto append = [&](const char* type, const QJsonObject& payload) {
                ++events;
                return journal.appendEvent(session.id, 1, QString::fromLatin1(type), payload,
                                           &error) > 0;
            };
            const QString item = QStringLiteral("item-%1").arg(turn);
            const QJsonObject message{{"id", item},
                                      {"type", "agentMessage"},
                                      {"text", words(40 + int(next() % 200))},
                                      {"phase", "final_answer"}};
            if (!append("turn.started", {{"turn_id", QStringLiteral("turn-%1").arg(turn)}}) ||
                !append("user.message", {{"text", words(8 + int(next() % 30))}}) ||
                !append("item.started",
                        {{"item_id", item}, {"item_type", "agentMessage"}, {"item", message}}) ||
                !append("item.completed", {{"item_id", item},
                                           {"item_type", "agentMessage"},
                                           {"item", message},
                                           {"text", message.value("text")}}) ||
                !append("turn.completed", {{"status", "completed"}})) {
                std::fprintf(stderr, "append failed: %s\n", qPrintable(error));
                return 6;
            }
        }
        if (int(next() % 100) < archivedPercent) {
            journal.setSessionArchived(session.id, 1,
                                       start.addDays(170).toString(Qt::ISODateWithMs));
            ++archived;
        }
        if (index % 500 == 499)
            std::fprintf(stderr, "%d conversations\n", index + 1);
    }
    std::printf("conversations=%d repositories=%d archived=%d events=%d journal=%s\n",
                conversations, repositories, archived, events,
                qPrintable(out.filePath("journal.sqlite3")));
    return 0;
}
