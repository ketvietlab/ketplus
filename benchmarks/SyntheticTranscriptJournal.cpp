// Writes a daemon journal full of synthetic conversation for the transcript
// scroll benchmark, so it can be run and shared without anyone's real data.
//
// The shape follows what a long agent session actually produces -- runs of
// command executions that collapse into activity groups, reasoning, file
// changes, and assistant answers of very uneven length -- plus a Mermaid diagram
// every few turns, because a diagram is the one row whose height used to change
// after it was laid out.
//
// Usage: ketplus_synthetic_transcript_journal <outdir> [turns=120] [mermaidEvery=3]
// Writes <outdir>/journal.sqlite3 with one session titled "synthetic-scroll",
// whose worktree is <outdir>/worktree.

#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

using namespace ketplus::daemon;

namespace {

constexpr auto kSession = "synthetic-session";

quint32 gSeed = 20260916;
quint32 next() {
    gSeed = gSeed * 1664525u + 1013904223u;
    return gSeed >> 8;
}

QString sentence(const int index) {
    static const QStringList words{"window",  "row",      "height",   "frame",  "layout",
                                   "measure", "scroll",   "extent",   "anchor", "estimate",
                                   "prefix",  "viewport", "delegate", "model"};
    QString result = QStringLiteral("The");
    const int length = 8 + int(next() % 14);
    for (int word = 0; word < length; ++word)
        result += ' ' + words.at((index + word + int(next() % 5)) % words.size());
    return result + '.';
}

QString paragraphs(const int count) {
    QStringList result;
    for (int paragraph = 0; paragraph < count; ++paragraph) {
        QStringList sentences;
        const int length = 2 + int(next() % 6);
        for (int index = 0; index < length; ++index)
            sentences.append(sentence(paragraph * 7 + index));
        result.append(sentences.join(' '));
    }
    return result.join(QStringLiteral("\n\n"));
}

QString mermaid(const int turn) {
    QString diagram = QStringLiteral("```mermaid\nflowchart TD\n");
    const int nodes = 6 + int(next() % 24);
    for (int node = 0; node < nodes; ++node)
        diagram +=
            QStringLiteral("  T%1N%2[Stage %2] --> T%1N%3\n").arg(turn).arg(node).arg(node + 1);
    return diagram + QStringLiteral("```");
}

QString answer(const int turn, const bool withMermaid) {
    // Mostly short, with a long tail, as real answers are.
    const int size = next() % 10;
    QString body = paragraphs(size < 6 ? 1 + size / 2 : size < 9 ? 5 + size : 18);
    if (next() % 3 == 0)
        body +=
            QStringLiteral("\n\n```cpp\nint rowTop(int row) {\n    return tops_.at(row);\n}\n```");
    if (next() % 4 == 0)
        body += QStringLiteral("\n\n- first point\n- second point\n- third point");
    if (withMermaid)
        body += QStringLiteral("\n\n") + mermaid(turn) + QStringLiteral("\n\n") + paragraphs(1);
    return body;
}

struct Writer {
    LocalJournal& journal;
    int items{0};
    bool append(const char* type, const QJsonObject& payload) {
        QString error;
        if (journal.appendEvent(kSession, 1, QString::fromLatin1(type), payload, &error) > 0)
            return true;
        std::fprintf(stderr, "append %s failed: %s\n", type, qPrintable(error));
        return false;
    }
    bool item(const QJsonObject& body) {
        const QString id = QStringLiteral("item-%1").arg(++items);
        QJsonObject item = body;
        item.insert("id", id);
        QJsonObject started{{"item_id", id}, {"item_type", item.value("type")}, {"item", item}};
        QJsonObject completed = started;
        if (item.contains("status"))
            completed.insert("status", item.value("status"));
        if (item.contains("text"))
            completed.insert("text", item.value("text"));
        return append("item.started", started) && append("item.completed", completed);
    }
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: %s outdir [turns] [mermaidEvery]\n", argv[0]);
        return 2;
    }
    const QDir out(args.at(1));
    const int turns = args.size() > 2 ? args.at(2).toInt() : 120;
    const int mermaidEvery = args.size() > 3 ? args.at(3).toInt() : 3;
    out.mkpath(QStringLiteral("worktree"));
    QFile::remove(out.filePath("journal.sqlite3"));

    LocalJournal journal(out.filePath("journal.sqlite3"));
    if (!journal.isOpen()) {
        std::fprintf(stderr, "journal: %s\n", qPrintable(journal.errorString()));
        return 3;
    }
    StoredSession session;
    session.id = kSession;
    session.worktreePath = QFileInfo(out.filePath("worktree")).canonicalFilePath();
    session.adapter = QStringLiteral("codex");
    session.threadId = kSession;
    session.state = QStringLiteral("ready");
    session.title = QStringLiteral("synthetic-scroll");
    if (!journal.upsertSession(session))
        return 4;

    Writer writer{journal};
    int diagrams = 0;
    for (int turn = 1; turn <= turns; ++turn) {
        if (!writer.append("turn.started", {{"turn_id", QStringLiteral("turn-%1").arg(turn)}}) ||
            !writer.append("user.message", {{"text", paragraphs(1 + int(next() % 2))}}))
            return 5;
        writer.item(
            {{"type", "userMessage"},
             {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", sentence(turn)}}}}});

        // A run of tool activity: this is what collapses into command groups.
        const int activity = int(next() % 14);
        for (int step = 0; step < activity; ++step) {
            const int kind = int(next() % 10);
            if (kind < 2) {
                writer.item({{"type", "reasoning"},
                             {"summary", QJsonArray{sentence(step)}},
                             {"content", QJsonArray{}}});
            } else if (kind < 9) {
                const QString command = QStringLiteral("rg -n \"row%1\" modules/src").arg(step);
                QString output;
                for (int line = 0, lines = int(next() % 40); line < lines; ++line)
                    output += QStringLiteral("modules/src/file%1.cpp:%2: %3\n")
                                  .arg(line)
                                  .arg(line * 7)
                                  .arg(sentence(line));
                writer.item({{"type", "commandExecution"},
                             {"command", command},
                             {"cwd", "/synthetic/worktree"},
                             {"status", "completed"},
                             {"exitCode", 0},
                             {"durationMs", 40},
                             {"aggregatedOutput", output},
                             {"commandActions", QJsonArray{QJsonObject{{"type", "search"},
                                                                       {"command", command},
                                                                       {"query", "row"},
                                                                       {"path", "modules/src"}}}}});
            } else {
                writer.item(
                    {{"type", "fileChange"},
                     {"status", "completed"},
                     {"changes", QJsonArray{QJsonObject{
                                     {"path", QStringLiteral("modules/src/file%1.cpp").arg(step)},
                                     {"kind", QJsonObject{{"type", "update"}}},
                                     {"diff", "@@ -1 +1 @@\n-old\n+new\n"}}}}});
            }
        }

        const bool withMermaid = mermaidEvery > 0 && turn % mermaidEvery == 0;
        diagrams += withMermaid ? 1 : 0;
        writer.item({{"type", "agentMessage"},
                     {"text", answer(turn, withMermaid)},
                     {"phase", "final_answer"}});
        if (!writer.append("turn.completed", {{"status", "completed"}}))
            return 6;
    }
    std::printf("turns=%d items=%d diagrams=%d journal=%s\n", turns, writer.items, diagrams,
                qPrintable(out.filePath("journal.sqlite3")));
    return 0;
}
