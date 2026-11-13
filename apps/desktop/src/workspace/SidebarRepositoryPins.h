#pragma once

#include <QString>
#include <QStringList>

namespace ketplus {

// T-92: the repositories the user pinned to the top of the sidebar, in the order they were
// pinned, by canonical path. Kept per machine in QSettings (`sidebar/pinnedRepositories`);
// desktop only, never sent to a phone.
class SidebarRepositoryPins final {
  public:
    // Reads the pins and forgets, silently, those whose folder is gone.
    void load();
    // The place of `path` among the pins, from 0; -1 when it is not pinned.
    [[nodiscard]] int order(const QString& path) const;
    [[nodiscard]] bool isPinned(const QString& path) const { return order(path) >= 0; }
    // Pinning puts the repository after the ones pinned before it.
    void pin(const QString& path);
    void unpin(const QString& path);
    [[nodiscard]] const QStringList& paths() const { return paths_; }

    // The key a path is pinned under: its canonical path while the folder exists.
    [[nodiscard]] static QString key(const QString& path);

  private:
    void save() const;

    QStringList paths_;
};

} // namespace ketplus
