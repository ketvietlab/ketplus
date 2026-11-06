#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QLockFile;
class QTimer;

namespace ketplus::daemon {

class ClaudeAdapter;
class CodexAdapter;
class CursorAdapter;
class CloudControlClient;
class LocalIpcServer;
class LocalJournal;
class ClaudeCredentials;
class SessionHost;

struct DaemonConfiguration final {
    QString statePath;
    QString tokenPath;
    QString socketName;
    QString codexExecutable;
    QString claudeExecutable;
    QString cursorExecutable;
};

class DaemonService final : public QObject {
    Q_OBJECT

  public:
    explicit DaemonService(DaemonConfiguration configuration, QObject* parent = nullptr);
    ~DaemonService() override;

    [[nodiscard]] bool start();
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] static DaemonConfiguration defaultConfiguration();

  private:
    // Free pages go back to the disk in batches while no turn runs (#128).
    static constexpr int reclaimIntervalMs = 60'000;
    static constexpr qint64 reclaimBatchPages = 4'096;

    [[nodiscard]] QByteArray loadOrCreateToken();
    void compactJournal();
    void reclaimJournalSpace();

    DaemonConfiguration configuration_;
    QString error_;
    std::unique_ptr<QLockFile> journalLock_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<CodexAdapter> adapter_;
    std::unique_ptr<ClaudeCredentials> claudeCredentials_;
    std::unique_ptr<ClaudeAdapter> claude_;
    std::unique_ptr<CursorAdapter> cursor_;
    std::unique_ptr<SessionHost> host_;
    std::unique_ptr<CloudControlClient> cloud_;
    std::unique_ptr<LocalIpcServer> ipc_;
    QTimer* spaceTimer_{nullptr};
};

} // namespace ketplus::daemon
