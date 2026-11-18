#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <sys/types.h>
#include <unistd.h>

// The runner's private directory: owned by the user running the runner, mode 0700, one
// process at a time (an exclusive lock on its `lock` file), every file 0600 and replaced
// atomically. Errors name the file, never its content.
namespace ketplus::runner::app {

struct Environment;

class StateDirectory {
  public:
    // --state-dir, then KETPLUS_RUNNER_STATE_DIR, then $XDG_STATE_HOME/ketplus-runner, then
    // ~/.local/state/ketplus-runner. Relative XDG_STATE_HOME is ignored as the spec requires.
    [[nodiscard]] static QString resolve(const QString& option, const Environment& environment);

    // Creates the directory (0700) when missing, checks its owner and mode, and takes the
    // lock. `owner` is the user the directory must belong to.
    [[nodiscard]] static std::optional<StateDirectory> open(const QString& path, QString* error,
                                                            uid_t owner = geteuid());

    StateDirectory(StateDirectory&& other) noexcept;
    StateDirectory& operator=(StateDirectory&& other) noexcept;
    StateDirectory(const StateDirectory&) = delete;
    StateDirectory& operator=(const StateDirectory&) = delete;
    ~StateDirectory();

    [[nodiscard]] const QString& path() const { return m_path; }

    struct ReadResult {
        enum class Status { Missing, Ok, Invalid };
        Status status = Status::Missing;
        QByteArray data;
        QString error;
    };
    // `name` is relative, such as "credential.json" or "pins/m1.json". A file that is not a
    // regular file owned by the directory's user with no group or other access is Invalid.
    [[nodiscard]] ReadResult read(const QString& name, qint64 maxBytes = 1024 * 1024) const;
    [[nodiscard]] bool exists(const QString& name) const;
    // Temporary file (0600), write, fsync, rename over `name`, fsync of the directory.
    [[nodiscard]] bool write(const QString& name, const QByteArray& data, QString* error) const;
    // Removing a missing file succeeds.
    [[nodiscard]] bool remove(const QString& name, QString* error) const;
    // Names of the regular files in a subdirectory, created 0700 when missing.
    [[nodiscard]] std::optional<QStringList> list(const QString& subdirectory,
                                                  QString* error) const;

  private:
    StateDirectory(QString path, int lockFd, uid_t owner)
        : m_path(std::move(path)), m_lockFd(lockFd), m_owner(owner) {}
    [[nodiscard]] QString absolute(const QString& name) const;
    [[nodiscard]] bool ensureSubdirectory(const QString& name, QString* error) const;

    QString m_path;
    int m_lockFd = -1;
    uid_t m_owner = 0;
};

} // namespace ketplus::runner::app
