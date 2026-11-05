#include "runtime/CodexItems.h"

#include <QJsonArray>
#include <QtTest>

using ketplus::daemon::withCommandDescription;

namespace {

QJsonObject command(const QJsonArray& actions, const QString& description = {}) {
    QJsonObject item{{"type", "commandExecution"},
                     {"command", "/bin/zsh -lc \"sed -n 1,80p src/a.cpp\""},
                     {"commandActions", actions}};
    if (!description.isEmpty())
        item.insert("description", description);
    return item;
}

} // namespace

class CodexItemsTest final : public QObject {
    Q_OBJECT

  private slots:
    // A Codex command says what it does the way a Claude command does, so the phone and the
    // desktop show a line above the command for both providers.
    void codexCommandsSayWhatTheyDo() {
        const auto describe = [](const QJsonArray& actions) {
            return withCommandDescription(command(actions)).value("description").toString();
        };
        QCOMPARE(describe({QJsonObject{{"type", "read"}, {"name", "a.cpp"}, {"path", "src/a.cpp"}}}),
                 QStringLiteral("Read a.cpp"));
        QCOMPARE(describe({QJsonObject{{"type", "read"}, {"path", "/w/src/a.cpp"}},
                           QJsonObject{{"type", "read"}, {"path", "/w/src/b.cpp"}}}),
                 QStringLiteral("Read 2 files"));
        QCOMPARE(describe({QJsonObject{{"type", "search"}, {"query", "stopTask"}, {"path", "src"}}}),
                 QStringLiteral("Search for “stopTask” in src"));
        QCOMPARE(describe({QJsonObject{{"type", "listFiles"}, {"path", "apps"}}}),
                 QStringLiteral("List files in apps"));
        QCOMPARE(describe({QJsonObject{{"type", "read"}, {"name", "a.cpp"}},
                           QJsonObject{{"type", "search"}, {"query", "x"}}}),
                 QStringLiteral("Read a.cpp, Search for “x”"));
    }

    // Codex could not read the command, and a provider's own words always win.
    void unknownCommandsAndProviderWordsStayAsTheyAre() {
        const QJsonObject unknown =
            withCommandDescription(command({QJsonObject{{"type", "unknown"}, {"command", "gh pr view"}}}));
        QVERIFY(!unknown.contains("description"));
        QCOMPARE(withCommandDescription(
                     command({QJsonObject{{"type", "read"}, {"name", "a.cpp"}}}, "Inspect the parser"))
                     .value("description")
                     .toString(),
                 QStringLiteral("Inspect the parser"));
        const QJsonObject message{{"type", "agentMessage"}, {"text", "hi"}};
        QCOMPARE(withCommandDescription(message), message);
    }
};

QTEST_GUILESS_MAIN(CodexItemsTest)
#include "CodexItemsTest.moc"
