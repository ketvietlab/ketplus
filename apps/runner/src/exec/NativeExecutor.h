#pragma once

#include "exec/ExecPorts.h"

// The job executor installed beside the runner: its client starts and stops the supervisor
// process, and its output store is read from the job's directory.
namespace ketplus::runner::exec {

class NativeExecutor final : public Executor {
  public:
    // `program`: the executor's supervisor program.
    explicit NativeExecutor(QString program);

    // The supervisor program next to the running executable.
    [[nodiscard]] static QString installedProgram();

    [[nodiscard]] std::unique_ptr<ExecutorJob> job(const QString& directory, const QString& taskId,
                                                   const QString& lease) override;
    [[nodiscard]] std::unique_ptr<OutputSpool> output(const QString& directory,
                                                      const QString& taskId) override;

  private:
    QString m_program;
};

} // namespace ketplus::runner::exec
