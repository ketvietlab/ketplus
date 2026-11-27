#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Each job's own /tmp, kept on disk inside the work directory beside the workspaces: owner-only,
// named by the job's executor task id, removed once the job's cleanup is confirmed.
namespace ketplus::runner::exec {

class JobTmpDirectories {
  public:
    // `root`: the directory holding them, inside the work directory.
    explicit JobTmpDirectories(QString root);

    // A new, empty directory for `taskId`; what an earlier attempt left under the name is removed
    // first. Empty when it cannot be made.
    [[nodiscard]] std::optional<QString> create(const QString& taskId);
    // Every name in the root, whatever it holds.
    [[nodiscard]] QStringList names() const;
    // Removes the directory named `name` and everything in it, whatever its permissions; symbolic
    // links are removed, never followed. True when nothing is left.
    bool remove(const QString& name) const;

  private:
    [[nodiscard]] QString canonicalRoot() const;

    QString m_root;
};

} // namespace ketplus::runner::exec
