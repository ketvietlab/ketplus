#include "runtime/JournalStorageJobs.h"

#include "storage/JournalStorageReader.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <optional>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& text) {
    return {{"ok", false}, {"error_code", code}, {"error_message", text}};
}
} // namespace

JournalStorageJobs::JournalStorageJobs(LocalJournal* journal, Hooks hooks, QObject* parent)
    : QObject(parent), journal_(journal), hooks_(std::move(hooks)) {
    compactionTimer_.setSingleShot(false);
    connect(&compactionTimer_, &QTimer::timeout, this, &JournalStorageJobs::compactionPass);
}

JournalStorageJobs::~JournalStorageJobs() {
    if (exportProgress_)
        exportProgress_->total = -1; // asks a running export to stop
    for (QThread* thread : {breakdownThread_, exportThread_})
        if (thread)
            thread->wait();
}

qint64 JournalStorageJobs::databaseBytes() const {
    const auto space = journal_->space();
    return space ? space->pageSize * space->pageCount : 0;
}

QJsonObject JournalStorageJobs::handle(const QString& type, const QJsonObject& payload) {
    if (type == QStringLiteral("storage.compaction.start") && payload.isEmpty()) {
        if (!compaction_.running) {
            compaction_ = {};
            compaction_.running = true;
            compaction_.phase = QStringLiteral("starting");
            compaction_.startedAt = QDateTime::currentDateTimeUtc();
            compaction_.beforeBytes = databaseBytes();
            compaction_.sweepsAtStart = journal_->completedDetailSweeps();
            compaction_.sweepsAtLastChange = compaction_.sweepsAtStart;
            compactionTimer_.start(0);
        }
        return {{"ok", true}, {"compaction", compactionStatus()}};
    }
    if (type == QStringLiteral("storage.compaction.read") && payload.isEmpty())
        return {{"ok", true}, {"compaction", compactionStatus()}};
    if (type == QStringLiteral("storage.breakdown.read") && payload.isEmpty())
        return breakdown();
    if (type == QStringLiteral("storage.export.start"))
        return startExport(payload);
    if (type == QStringLiteral("storage.export.read") && payload.isEmpty())
        return {{"ok", true}, {"export", exportStatus()}};
    return failure("invalid_request", "Unsupported storage operation or parameters.");
}

QJsonObject JournalStorageJobs::compactionStatus() const {
    QJsonObject status{{"state", compaction_.running ? (compaction_.phase == "waiting_for_idle"
                                                            ? QStringLiteral("waiting_for_idle")
                                                            : QStringLiteral("running"))
                                                     : QStringLiteral("idle")}};
    if (compaction_.running || !compaction_.outcome.isEmpty()) {
        status.insert("started_at", compaction_.startedAt.toString(Qt::ISODateWithMs));
        status.insert("before_bytes", compaction_.beforeBytes);
        status.insert("database_bytes", databaseBytes());
    }
    if (compaction_.running) {
        status.insert("sweeps", journal_->completedDetailSweeps() - compaction_.sweepsAtStart);
        status.insert("sweep_progress", journal_->detailSweepProgress());
    }
    if (!compaction_.outcome.isEmpty())
        status.insert("outcome", compaction_.outcome);
    if (!compaction_.error.isEmpty())
        status.insert("error", compaction_.error);
    if (const auto last = journal_->lastCompaction(); last && !last->isEmpty())
        status.insert("last", *last);
    return status;
}

void JournalStorageJobs::compactionPass() {
    if (!compaction_.running) {
        compactionTimer_.stop();
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (compaction_.startedAt.secsTo(now) > compactionLimitMinutes * 60) {
        finishCompaction(QStringLiteral("stopped"));
        return;
    }
    // A running turn keeps working; compaction waits for it, as the idle pass does.
    if (hooks_.busy && hooks_.busy()) {
        compaction_.phase = QStringLiteral("waiting_for_idle");
        compactionTimer_.setInterval(1000);
        return;
    }
    compaction_.phase = QStringLiteral("compacting");
    compactionTimer_.setInterval(10); // let client requests in between passes
    journal_->pruneAcknowledgedEvents();
    journal_->purgeExpiredAttachments();
    journal_->pruneRetention();
    const auto reclaimed = journal_->reclaimFreePages(compactionReclaimPages);
    const bool expired = hooks_.expireArchived && hooks_.expireArchived(now);
    // A large journal has millions of rows: sweep batch after batch within the budget, and
    // stop early at the end of a sweep so the done check below sees it.
    std::optional<qint64> changed = 0;
    QElapsedTimer budget;
    budget.start();
    const auto sweepsBefore = journal_->completedDetailSweeps();
    do {
        const auto batch = journal_->pruneConfiguredReplayDetails(now, compactionRows);
        if (!batch) {
            changed.reset();
            break;
        }
        *changed += *batch;
    } while (journal_->completedDetailSweeps() == sweepsBefore &&
             budget.elapsed() < compactionBudgetMs);
    if (!reclaimed || !changed) {
        // Another writer held the journal; a few misses in a row are normal.
        if (++compaction_.failures >= 50) {
            compaction_.error = journal_->errorString();
            finishCompaction(QStringLiteral("failed"));
        }
        return;
    }
    compaction_.failures = 0;
    const auto sweeps = journal_->completedDetailSweeps();
    if (*reclaimed > 0 || *changed > 0 || expired) {
        compaction_.sweepsAtLastChange = sweeps;
        return;
    }
    // Done once a whole sweep after the last change found nothing left to trim or return.
    if (sweeps >= compaction_.sweepsAtLastChange + 2)
        finishCompaction(QStringLiteral("done"));
}

void JournalStorageJobs::finishCompaction(const QString& outcome) {
    compactionTimer_.stop();
    compaction_.running = false;
    compaction_.phase.clear();
    compaction_.outcome = outcome;
    if (outcome == QStringLiteral("done") &&
        !journal_->recordCompaction(QDateTime::currentDateTimeUtc(), compaction_.beforeBytes,
                                    databaseBytes()))
        compaction_.error = journal_->errorString();
    breakdownAt_ = {}; // the parts changed; measure again on the next read
}

void JournalStorageJobs::startBreakdown() {
    if (breakdownThread_)
        return;
    const QString path = journal_->databasePath();
    breakdownThread_ = QThread::create([this, path] {
        QString error;
        auto result = journal_reader::measure(path, &error);
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result), error] {
                breakdownThread_->wait();
                breakdownThread_->deleteLater();
                breakdownThread_ = nullptr;
                breakdownAt_ = QDateTime::currentDateTimeUtc();
                breakdownError_ = result ? QString() : error;
                if (result)
                    breakdown_ = *result;
            },
            Qt::QueuedConnection);
    });
    breakdownThread_->setObjectName(QStringLiteral("ketplus-journal-breakdown"));
    breakdownThread_->start(QThread::LowPriority);
}

QJsonObject JournalStorageJobs::breakdown() {
    const bool stale =
        !breakdownAt_.isValid() ||
        breakdownAt_.secsTo(QDateTime::currentDateTimeUtc()) > breakdownMaximumAgeSeconds;
    if (stale && !compaction_.running)
        startBreakdown();
    QJsonObject result{
        {"ok", true},
        {"state", breakdownThread_ ? QStringLiteral("measuring") : QStringLiteral("ready")}};
    if (!breakdown_.isEmpty())
        result.insert("breakdown", breakdown_);
    if (!breakdownError_.isEmpty())
        result.insert("error", breakdownError_);
    return result;
}

QJsonObject JournalStorageJobs::startExport(const QJsonObject& payload) {
    const auto directory = payload.value("directory").toString();
    if (payload.size() != 1 || !payload.value("directory").isString() ||
        !QFileInfo(directory).isAbsolute() || !QFileInfo(directory).isDir())
        return failure("invalid_request", "Supply the absolute path of an existing folder.");
    if (!exportThread_) {
        exportProgress_ = std::make_shared<ExportProgress>();
        exportResult_ = {{"state", "running"}};
        const QString path = journal_->databasePath();
        const auto progress = exportProgress_;
        exportThread_ = QThread::create([this, path, directory, progress] {
            QString error;
            auto result = journal_reader::exportConversations(
                path, QDir::cleanPath(directory),
                [progress](int done, int total) {
                    if (progress->total.load() < 0)
                        return false;
                    progress->done = done;
                    progress->total = total;
                    return true;
                },
                &error);
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result), error] {
                    exportThread_->wait();
                    exportThread_->deleteLater();
                    exportThread_ = nullptr;
                    exportResult_ = result ? *result : QJsonObject{{"error", error}};
                    exportResult_.insert("state", result ? "done" : "failed");
                },
                Qt::QueuedConnection);
        });
        exportThread_->setObjectName(QStringLiteral("ketplus-journal-export"));
        exportThread_->start(QThread::LowPriority);
    }
    return {{"ok", true}, {"export", exportStatus()}};
}

QJsonObject JournalStorageJobs::exportStatus() const {
    auto status = exportResult_.isEmpty() ? QJsonObject{{"state", "idle"}} : exportResult_;
    if (exportThread_ && exportProgress_) {
        status.insert("exported", exportProgress_->done.load());
        status.insert("total", qMax(0, exportProgress_->total.load()));
    }
    return status;
}
} // namespace ketplus::daemon
