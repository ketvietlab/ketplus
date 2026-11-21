#pragma once

#include <QString>

#include <optional>
#include <sys/types.h>
#include <unistd.h>

// Where the runner keeps job workspaces: private to its user, outside the state directory, and
// held by one run at a time through `.lock` inside it.
namespace ketplus::runner::app {

struct Environment;

class WorkDirectory {
  public:
    // --work-dir, else $XDG_CACHE_HOME/ketplus-runner/<runnerId>/work, else
    // ~/.cache/ketplus-runner/<runnerId>/work. Empty when none can be named.
    [[nodiscard]] static QString resolve(const QString& option, const QString& runnerId,
                                         const Environment& environment);
    // Creates the directory with mode 0700 or checks the one there, then locks it.
    [[nodiscard]] static std::optional<WorkDirectory>
    open(const QString& path, const QString& statePath, QString* error, uid_t owner = geteuid());

    WorkDirectory(WorkDirectory&& other) noexcept;
    WorkDirectory& operator=(WorkDirectory&& other) noexcept;
    WorkDirectory(const WorkDirectory&) = delete;
    WorkDirectory& operator=(const WorkDirectory&) = delete;
    ~WorkDirectory();

    [[nodiscard]] const QString& path() const { return m_path; }

  private:
    WorkDirectory(QString path, int lockFd) : m_path(std::move(path)), m_lockFd(lockFd) {}

    QString m_path;
    int m_lockFd = -1;
};

} // namespace ketplus::runner::app
