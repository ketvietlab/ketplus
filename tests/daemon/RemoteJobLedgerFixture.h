#pragma once

#include "runner/jobs/LedgerRecordStore.h"
#include "storage/BackgroundTaskLedger.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>

// A background task registry in a temporary SQLite file with its remote job store, reopened as
// after a daemon restart.
namespace remote_job_ledger_test {

using namespace ketplus::daemon;
using namespace ketplus::daemon::runner::jobs;

inline const QStringList linuxLabels = {QStringLiteral("os:linux")};
inline const BackgroundTaskOwner owner{QStringLiteral("owner"), 1, QStringLiteral("turn")};

inline BackgroundTaskIntent intentFor(const QString& request,
                                      const std::optional<QStringList>& runsOn = linuxLabels,
                                      const qint64 timeoutMs = 60'000) {
    BackgroundTaskIntent intent{
        owner,     request, QStringLiteral("process"), "Build", QByteArray("fingerprint"),
        timeoutMs, {}};
    intent.runsOn = runsOn;
    return intent;
}

inline QString taskIdOf(const QJsonObject& reply) {
    return reply.value("task").toObject().value("task_id").toString();
}

struct LedgerFixture {
    QTemporaryDir directory;
    QString connection;
    QSqlDatabase database;
    std::unique_ptr<BackgroundTaskLedger> ledger;
    std::unique_ptr<LedgerRecordStore> store;
    int requests = 0;

    LedgerFixture() { open(); }
    ~LedgerFixture() { close(); }
    LedgerFixture(const LedgerFixture&) = delete;
    LedgerFixture& operator=(const LedgerFixture&) = delete;

    [[nodiscard]] QString path() const { return directory.filePath("registry.sqlite"); }
    bool open() {
        connection = QUuid::createUuid().toString();
        database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path());
        QString error;
        QSqlQuery query(database);
        if (!database.open() || !query.exec("PRAGMA foreign_keys=ON"))
            return false;
        ledger = std::make_unique<BackgroundTaskLedger>(database);
        store = std::make_unique<LedgerRecordStore>(database, *ledger);
        return ledger->initialize(&error) &&
               query.exec("CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,"
                          "generation INTEGER)") &&
               query.exec("INSERT OR IGNORE INTO sessions VALUES('owner',1)");
    }
    void close() {
        store.reset();
        ledger.reset();
        database.close();
        database = {};
        QSqlDatabase::removeDatabase(connection);
    }
    // The same file through a new connection, registry and store.
    LedgerRecordStore* reopen() {
        close();
        return open() ? store.get() : nullptr;
    }
    bool exec(const QString& statement) {
        QSqlQuery query(database);
        return query.exec(statement);
    }
    // A task reserved for a remote job and approved; empty on failure.
    QString remoteTask(const QStringList& runsOn = linuxLabels, const qint64 timeoutMs = 60'000) {
        const QString id = taskIdOf(ledger->create(
            intentFor(QStringLiteral("request-%1").arg(++requests), runsOn, timeoutMs)));
        return id.isEmpty() || !approve(id) ? QString() : id;
    }
    bool approve(const QString& id) {
        BackgroundTaskChange change;
        change.state = QStringLiteral("queued");
        change.permissionApproved = true;
        return ledger->change(id, revision(id), change).value("ok").toBool();
    }
    [[nodiscard]] QJsonObject task(const QString& id) const { return ledger->runtimeTask(id); }
    [[nodiscard]] qint64 revision(const QString& id) const {
        return task(id).value("revision").toInteger();
    }
    [[nodiscard]] QString state(const QString& id) const {
        return task(id).value("state").toString();
    }
    [[nodiscard]] int count(const QString& sql) {
        QSqlQuery query(database);
        return query.exec(sql) && query.next() ? query.value(0).toInt() : -1;
    }
    // Every later write of a remote record fails inside its transaction, or works again.
    void failRemoteWrites(const bool fail) {
        exec(fail ? "CREATE TEMP TRIGGER IF NOT EXISTS fail_remote BEFORE UPDATE ON remote_jobs "
                    "BEGIN SELECT RAISE(ABORT,'injected'); END"
                  : "DROP TRIGGER IF EXISTS temp.fail_remote");
    }
};

} // namespace remote_job_ledger_test
