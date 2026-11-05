#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

namespace ketplus {

struct RepositoryMetadata {
    QString path;
    QString identity;
    QString root;
};

// Disposable desktop navigation cache, separate from the daemon journal.
// Each operation owns its SQLite connection on the calling thread.
class RepositoryMetadataStore final {
  public:
    explicit RepositoryMetadataStore(QString databasePath = {});
    std::optional<RepositoryMetadata> lookup(const QString& path) const;
    // Entries for the paths that have valid cached metadata, read over one connection.
    QHash<QString, RepositoryMetadata> lookupAll(const QStringList& paths) const;
    void save(const RepositoryMetadata& metadata) const;
    void remove(const QString& path) const;
    // The checkout that owns `path`: the nearest folder at or above it with a `.git` entry,
    // canonical. Empty when there is none (a bare repository or no repository).
    static QString checkoutRoot(const QString& path);

  private:
    QString databasePath_;
};

} // namespace ketplus
