#pragma once

#include "exec/ExecPorts.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

// Whether a Linux machine can contain a job: the runner's account, the user session the job
// executor needs, and a real run of the executor's own probes. Nothing here changes the machine.
namespace ketplus::runner::exec {

// What the policy reads of the machine; tests replace it.
class LinuxHost {
  public:
    virtual ~LinuxHost() = default;
    [[nodiscard]] virtual uint effectiveUid() const = 0;
    [[nodiscard]] virtual QString bootId() const = 0;
    // A regular file the caller may execute.
    [[nodiscard]] virtual bool executable(const QString& path) const = 0;
    // XDG_RUNTIME_DIR, or empty.
    [[nodiscard]] virtual QString runtimeDirectory() const = 0;
    // The owner of a directory that is not a symbolic link.
    [[nodiscard]] virtual std::optional<uint> directoryOwner(const QString& path) const = 0;
    [[nodiscard]] virtual bool socket(const QString& path) const = 0;
    [[nodiscard]] virtual bool file(const QString& path) const = 0;
    // The real path of an existing directory, or empty.
    [[nodiscard]] virtual QString realDirectory(const QString& path) const = 0;
    // The executor's containment probe, run as a user unit of the session.
    virtual void probeContainment(const QString& program, std::function<void(bool)> done) = 0;
    // The executor's probe of a job's mounts, run the same way.
    virtual void probeMounts(const QString& program, std::function<void(bool)> done) = 0;
};

class LinuxPolicy final : public PlatformPolicy {
  public:
    // `program`: the executor's supervisor program. `runnerDirectories`: the runner's own
    // directories a job's mounts hide; with the user runtime directory, none may lie inside
    // another.
    LinuxPolicy(LinuxHost& host, QString program, QStringList runnerDirectories = {});
    ~LinuxPolicy() override;

    // Each check in order; the first that fails answers `containment_unavailable`, and only the
    // name of its step is logged.
    void probe(std::function<void(bool, const QString&)> done) override;

  private:
    void answer(const std::function<void(bool, const QString&)>& done, const char* failedStep);

    LinuxHost& m_host;
    QString m_program;
    QStringList m_directories;
    std::shared_ptr<bool> m_alive;
};

} // namespace ketplus::runner::exec
