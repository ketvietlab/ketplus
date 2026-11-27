#include "storage/LocalJournal.h"
#include <QCryptographicHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
SessionPreparation allocation(const QString& worktree, const QString& request = "draft-1") {
    return {QUuid::createUuid().toString(QUuid::WithoutBraces),
            "device-a",
            request,
            worktree,
            {{"provider", "codex"}},
            sessionPreparationTtlMs,
            "prepared"};
}
StoredSession activated(const SessionPreparation& preparation) {
    StoredSession session;
    session.id = preparation.sessionId;
    session.worktreePath = preparation.worktreePath;
    session.adapter = "codex";
    session.state = "ready";
    return session;
}
void stage(LocalJournal& journal, const QString& id) {
    const QByteArray bytes = "draft bytes";
    const auto digest =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QVERIFY(journal.beginAttachment(id, 1, "file-1", 0, "draft.txt", "text/plain", bytes.size(), 1,
                                    digest));
    QVERIFY(journal.appendAttachmentChunk(id, 1, "file-1", 1, 0, bytes, digest));
    QVERIFY(journal.commitAttachment(id, 1, "file-1", 1));
}
} // namespace
class SessionPreparationStoreTest final : public QObject {
    Q_OBJECT
  private slots:
    void privateAllocationSurvivesRestartAndRetry() {
        QTemporaryDir root;
        QString id;
        {
            LocalJournal journal(root.filePath("journal.sqlite"));
            auto preparation = allocation(root.path());
            QVERIFY(journal.prepareSession(preparation));
            id = preparation.sessionId;
            QVERIFY(journal.sessions().isEmpty());
            QVERIFY(journal.sessionPreparations().size() == 1);
        }
        LocalJournal reopened(root.filePath("journal.sqlite"));
        auto retry = allocation(root.path());
        retry.expiresAt *= 2;
        QVERIFY(reopened.prepareSession(retry));
        QCOMPARE(retry.sessionId, id);
        QCOMPARE(retry.expiresAt, sessionPreparationTtlMs);
        QVERIFY(reopened.sessions().isEmpty());
    }
    void requestCannotBeReboundToAnotherScope() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto preparation = allocation(root.path());
        QVERIFY(journal.prepareSession(preparation));
        auto changed = allocation(root.path() + "/other");
        QString error;
        QVERIFY(!journal.prepareSession(changed, &error));
        QCOMPARE(error, "preparation_request_conflict");
        changed = allocation(root.path());
        changed.options.insert("provider", "claude");
        QVERIFY(!journal.prepareSession(changed, &error));
        QCOMPARE(journal.sessionPreparations().size(), 1);
    }
    void ownerMustMatchBeforeActivationOrDiscard() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto preparation = allocation(root.path());
        QVERIFY(journal.prepareSession(preparation));
        QString error;
        QVERIFY(!journal.activatePreparedSession(activated(preparation), "device-b", 1, &error));
        QCOMPARE(error, "preparation_not_found");
        QVERIFY(!journal.discardPreparedSession(preparation.sessionId, "device-b", 1, &error));
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.sessionPreparation(preparation.sessionId)->state, "prepared");
    }
    void activationIsAtomicAndCannotBeDiscardedLater() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto preparation = allocation(root.path());
        QVERIFY(journal.prepareSession(preparation));
        stage(journal, preparation.sessionId);
        QVERIFY(journal.activatePreparedSession(activated(preparation), preparation.owner, 1));
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessions().first().lastCursor, 0);
        QCOMPARE(journal.sessionPreparation(preparation.sessionId)->state, "activated");
        QVERIFY(!journal.discardPreparedSession(preparation.sessionId, preparation.owner, 2));
        QVERIFY(!journal.activatePreparedSession(activated(preparation), preparation.owner, 2));
        QCOMPARE(journal.attachment(preparation.sessionId, 1, "file-1").status, "ready");
    }
    void discardAndExpiryRemoveStagedBytesAndKeepTerminalReceipt_data() {
        QTest::addColumn<qint64>("now");
        QTest::addColumn<QString>("state");
        QTest::newRow("cancel") << qint64(1) << QString("discarded");
        QTest::newRow("expire") << sessionPreparationTtlMs << QString("expired");
    }
    void discardAndExpiryRemoveStagedBytesAndKeepTerminalReceipt() {
        QFETCH(qint64, now);
        QFETCH(QString, state);
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto preparation = allocation(root.path());
        QVERIFY(journal.prepareSession(preparation));
        stage(journal, preparation.sessionId);
        QVERIFY(journal.discardPreparedSession(preparation.sessionId, preparation.owner, now));
        QVERIFY(journal.discardPreparedSession(preparation.sessionId, preparation.owner, now));
        QVERIFY(journal.sessionPreparations().isEmpty());
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.sessionPreparation(preparation.sessionId)->state, state);
        QVERIFY(journal.attachment(preparation.sessionId, 1, "file-1").status.isEmpty());
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(journal.databasePath());
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT COUNT(*) FROM staged_attachment_chunks"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        auto retry = allocation(root.path());
        QVERIFY(journal.prepareSession(retry));
        QCOMPARE(retry.sessionId, preparation.sessionId);
        QCOMPARE(retry.state, state);
        QVERIFY(!journal.activatePreparedSession(activated(preparation), preparation.owner, now));
    }
    void logicalExpiryDoesNotWaitForTheSweep() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto preparation = allocation(root.path());
        QVERIFY(journal.prepareSession(preparation));
        QString error;
        QVERIFY(!journal.activatePreparedSession(activated(preparation), preparation.owner,
                                                 sessionPreparationTtlMs, &error));
        QCOMPARE(error, "preparation_expired");
        QVERIFY(journal.sessions().isEmpty());
    }
    void activeAllocationsAreBoundedButAnIdempotentRetryStillWorks() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        QString firstId;
        for (int i = 0; i < 128; ++i) {
            auto preparation = allocation(root.path(), "draft-" + QString::number(i));
            QVERIFY(journal.prepareSession(preparation));
            if (i == 0)
                firstId = preparation.sessionId;
        }
        auto extra = allocation(root.path(), "draft-extra");
        QString error;
        QVERIFY(!journal.prepareSession(extra, &error));
        QCOMPARE(error, "preparation_capacity");
        auto retry = allocation(root.path(), "draft-0");
        QVERIFY(journal.prepareSession(retry));
        QCOMPARE(retry.sessionId, firstId);
        extra.owner = "device-b";
        QVERIFY(journal.prepareSession(extra));
        QVERIFY(journal.sessions().isEmpty());
    }
    void terminalReceiptsSurviveTheRetryWindowAndArePrunedInBoundedBatches() {
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite"));
        auto first = allocation(root.path(), "draft-1");
        auto second = allocation(root.path(), "draft-2");
        QVERIFY(journal.prepareSession(first));
        QVERIFY(journal.prepareSession(second));
        QVERIFY(journal.discardPreparedSession(first.sessionId, first.owner, 1));
        QVERIFY(journal.discardPreparedSession(second.sessionId, second.owner, 1));
        const qint64 cutoff = sessionPreparationTtlMs + 7LL * 24 * 60 * 60 * 1000;
        QVERIFY(journal.pruneSessionPreparations(cutoff - 1));
        QVERIFY(journal.sessionPreparation(first.sessionId).has_value());
        QVERIFY(journal.sessionPreparation(second.sessionId).has_value());
        QVERIFY(journal.pruneSessionPreparations(cutoff, 1));
        QCOMPARE(int(journal.sessionPreparation(first.sessionId).has_value()) +
                     int(journal.sessionPreparation(second.sessionId).has_value()),
                 1);
        QVERIFY(journal.pruneSessionPreparations(cutoff, 1));
        QVERIFY(!journal.sessionPreparation(first.sessionId));
        QVERIFY(!journal.sessionPreparation(second.sessionId));
    }
};
QTEST_GUILESS_MAIN(SessionPreparationStoreTest)
#include "SessionPreparationStoreTest.moc"
