#pragma once

// Test-only native failure cleanup. The caller keeps Host/journal/directory alive
// until this returns; uncertainty retains the fixture instead of erasing receipts.
#include "ManagedTaskIntentLookup.h"
#include "runtime/executor/NativeProcessIdentity.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <csignal>
#include <functional>

namespace attention_fixture {
struct CleanupResult {
    bool closed{false};
    bool trusted{true};
    bool stopFailed{false};
    int signalFailures{0};
    int capturedIdentities{0};
    bool ok() const { return closed && trusted && !stopFailed && signalFailures == 0; }
};

inline CleanupResult
cleanupNativeAttention(ketplus::daemon::LocalJournal& journal, QString& id, const QString& cwd,
                       const std::function<QJsonObject(const QString&)>& stop) {
    using namespace ketplus::daemon;
    CleanupResult result;
    auto ledger = journal.backgroundTasks();
    if (id.isEmpty()) {
        const auto lookup =
            resolveCleanupIntent(journal.databasePath(), "owner", 1, "attention", id);
        if (lookup == IntentLookup::Error) {
            result.trusted = false;
            return result;
        }
        if (lookup == IntentLookup::Absent) {
            result.closed = true; // Successful journal read proves no accepted intent.
            return result;
        }
    }
    QHash<QString, executor::NativeProcessIdentity> captured;
    bool supervisorKnown = false;
    bool addressRegistered = false;
    QJsonObject record;
    const auto capture = [&](const QJsonObject& value, bool supervisor = false) {
        const auto pid = value.value("pid").toInteger();
        const auto birth = value.value("birth").toString();
        if (value.isEmpty())
            return;
        if (pid <= 0 || birth.isEmpty()) {
            result.trusted = false;
            return;
        }
        captured.insert(QString::number(pid) + ':' + birth, {pid, 0, 0, birth});
        supervisorKnown = supervisorKnown || supervisor;
    };
    const auto refresh = [&] {
        record = ledger.runtimeTask(id);
        if (record.value("schema_version").toInt() != 1 || record.value("task_id") != id) {
            result.trusted = false;
            return;
        }
        const auto execution = record.value("execution_identity").toObject();
        capture(execution.value("supervisor_identity").toObject(), true);
        capture(execution.value("root_identity").toObject());
        if (execution.isEmpty())
            return;
        const auto expected = cwd + "/background-jobs/" + id;
        if (execution.value("directory") != expected ||
            execution.value("executor_lease_identity").toString().isEmpty()) {
            result.trusted = false;
            return;
        }
        addressRegistered = true;
        QFile file(expected + "/executor.json");
        if (!file.exists())
            return; // A register/spawn gap is not proof that no executor exists.
        if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) ||
            file.size() > 64 * 1024) {
            result.trusted = false;
            return;
        }
        QJsonParseError error;
        const auto receipt = QJsonDocument::fromJson(file.read(64 * 1024), &error).object();
        if (error.error != QJsonParseError::NoError ||
            receipt.value("schema_version").toInt() != 1 || receipt.value("task_id") != id ||
            receipt.value("executor_lease_identity") !=
                execution.value("executor_lease_identity")) {
            result.trusted = false;
            return;
        }
        capture(receipt.value("supervisor_identity").toObject(), true);
        capture(receipt.value("root_identity").toObject());
        const auto known = receipt.value("known_processes").toArray();
        if (known.size() > 256) {
            result.trusted = false;
            return;
        }
        for (const auto value : known)
            capture(value.toObject());
    };
    const auto stopped = [](const executor::NativeProcessIdentity& value) {
        const auto current = executor::inspect(value.pid);
        return !current || current->birth != value.birth || current->zombie;
    };
    const auto closed = [&] {
        if (!result.trusted || !record.value("executor_stopped").toBool())
            return false;
        if (!addressRegistered) {
            const auto state = record.value("state").toString();
            const bool endedBeforeLaunch = state == "cancelled" || state == "failed" ||
                                           state == "timed_out" || state == "interrupted";
            // A queued intent's initial executor_stopped flag does not prevent
            // a later launch. Cancel it before accepting no-executor closure.
            return endedBeforeLaunch && record.value("cleanup_status") == "not_required";
        }
        if (!supervisorKnown)
            return false;
        for (const auto& value : captured)
            if (!stopped(value))
                return false;
        return true;
    };
    // Let initialize()'s deferred reconciliation install its executor client before
    // requesting Stop. Do not terminalize an unreconciled registered address.
    QTest::qWait(50);
    refresh();
    bool requestedStop = false;
    QHash<QString, bool> signalled;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 6000) {
        refresh(); // Capture journal/receipt identities even if running wait failed.
        if (closed()) {
            refresh(); // A stopped supervisor cannot register more running children.
            if (closed()) {
                result.closed = true;
                break;
            }
        }
        if (!requestedStop) {
            const auto reply = stop(id);
            result.stopFailed = !reply.value("ok").toBool();
            requestedStop = true;
        }
        if (elapsed.elapsed() >= 1000) {
            for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
                if (signalled.contains(it.key()) || stopped(it.value()))
                    continue;
                QString error;
                if (!executor::signalIdentity(it.value(), SIGKILL, &error))
                    ++result.signalFailures;
                signalled.insert(it.key(), true);
            }
        }
        QTest::qWait(50);
    }
    result.capturedIdentities = captured.size();
    return result;
}
} // namespace attention_fixture
