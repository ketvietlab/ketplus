#pragma once

#include "state/StateDirectory.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

// Where the snapshot receiver keeps mirrors, partial packs and workspaces, and the durable
// records that say who owns each and what it holds. Every key reaching a path has passed
// isKey first. The records are written atomically through the state directory.
//
//   <state>/repos/<repositoryKey>/repository.json        owner, mirror size
//   <state>/repos/<repositoryKey>/mirror.git             bare mirror (snapshot library)
//   <state>/repos/<repositoryKey>/workspaces/<key>.json  owner, snapshot, complete, last use
//   <state>/repos/<repositoryKey>/partial/<key>.pack     pack being received, and its .json
//   <work>/<repositoryKey>/<workspaceKey>/               workspace: plain files, no .git
namespace ketplus::runner::receiver {

struct RepositoryRecord {
    QString repositoryKey;
    QString owner; // machineId of the desktop that created it
    qint64 mirrorBytes = 0;
};

struct WorkspaceRecord {
    QString repositoryKey;
    QString workspaceKey;
    QString owner;
    QString snapshotId; // the last snapshot applied whole
    bool complete = false;
    QDateTime lastUsed;
    qint64 bytes = 0;
};

struct PartialRecord {
    QString repositoryKey;
    QString workspaceKey;
    QString packSha256;
    qint64 packBytes = 0;
    qint64 offset = 0; // bytes written and synced
    QDateTime lastWrite;
};

struct StoredRecords {
    QList<RepositoryRecord> repositories;
    QList<WorkspaceRecord> workspaces;
    QList<PartialRecord> partials;
    QStringList orphanRepositories; // a directory without a readable record
};

class ReceiverStore {
  public:
    ReceiverStore(const app::StateDirectory& state, QString workRoot);

    [[nodiscard]] static bool isKey(const QString& key);

    [[nodiscard]] QString repositoryDirectory(const QString& repositoryKey) const;
    [[nodiscard]] QString mirrorPath(const QString& repositoryKey) const;
    [[nodiscard]] QString workspacePath(const QString& repositoryKey,
                                        const QString& workspaceKey) const;
    [[nodiscard]] QString partialPackPath(const QString& repositoryKey,
                                          const QString& workspaceKey) const;

    // Every record under the state directory; an unreadable one is left out.
    [[nodiscard]] StoredRecords load() const;

    [[nodiscard]] bool write(const RepositoryRecord& record, QString* error) const;
    [[nodiscard]] bool write(const WorkspaceRecord& record, QString* error) const;
    [[nodiscard]] bool write(const PartialRecord& record, QString* error) const;
    bool removeWorkspaceRecord(const QString& repositoryKey, const QString& workspaceKey) const;
    // The partial pack and its record.
    bool removePartial(const QString& repositoryKey, const QString& workspaceKey) const;
    // The repository's state directory (mirror, records, partial packs) and its work directory.
    bool removeRepository(const QString& repositoryKey) const;

  private:
    [[nodiscard]] std::optional<RepositoryRecord> readRepository(const QString& key) const;
    [[nodiscard]] std::optional<WorkspaceRecord> readWorkspace(const QString& repositoryKey,
                                                               const QString& name) const;
    [[nodiscard]] std::optional<PartialRecord> readPartial(const QString& repositoryKey,
                                                           const QString& name) const;

    const app::StateDirectory& m_state;
    QString m_workRoot;
};

} // namespace ketplus::runner::receiver
