#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace ketplus::daemon {
class LocalJournal;

// Settings → Storage work that takes longer than one request (ADR 0108):
// - Compact now runs the idle maintenance pass back to back until one whole detail sweep
//   changes nothing and the free pages are back on the disk;
// - the breakdown and the export read the journal on a worker thread through their own
//   read-only connection, so the daemon keeps serving clients meanwhile.
// Clients poll for progress; nothing here starts a provider.
class JournalStorageJobs final : public QObject {
    Q_OBJECT
  public:
    struct Hooks final {
        std::function<bool()> busy; // a session is starting, running or waiting
        std::function<bool(const QDateTime&)> expireArchived;
    };
    JournalStorageJobs(LocalJournal* journal, Hooks hooks, QObject* parent = nullptr);
    ~JournalStorageJobs() override;

    // storage.compaction.*, storage.breakdown.read and storage.export.*.
    QJsonObject handle(const QString& type, const QJsonObject& payload);
    [[nodiscard]] QJsonObject compactionStatus() const;

    static constexpr qint64 compactionReclaimPages = 32768; // 128 MB at 4 KiB pages
    static constexpr int compactionRows = 256;
    // Each pass sweeps batches of rows for this long, then lets client requests in.
    static constexpr int compactionBudgetMs = 50;
    static constexpr int compactionLimitMinutes = 30;
    static constexpr int breakdownMaximumAgeSeconds = 600;

  private:
    void compactionPass();
    void finishCompaction(const QString& outcome);
    void startBreakdown();
    QJsonObject breakdown();
    QJsonObject startExport(const QJsonObject& payload);
    QJsonObject exportStatus() const;
    [[nodiscard]] qint64 databaseBytes() const;

    LocalJournal* journal_;
    Hooks hooks_;
    QTimer compactionTimer_;
    struct Compaction final {
        bool running{false};
        QString phase;
        QDateTime startedAt;
        qint64 beforeBytes{0};
        qint64 sweepsAtStart{0};
        qint64 sweepsAtLastChange{0};
        int failures{0};
        QString outcome;
        QString error;
    } compaction_;
    QJsonObject breakdown_;
    QDateTime breakdownAt_;
    QString breakdownError_;
    QThread* breakdownThread_{nullptr};
    struct ExportProgress final {
        std::atomic<int> done{0};
        std::atomic<int> total{0};
    };
    std::shared_ptr<ExportProgress> exportProgress_;
    QThread* exportThread_{nullptr};
    QJsonObject exportResult_;
};
} // namespace ketplus::daemon
