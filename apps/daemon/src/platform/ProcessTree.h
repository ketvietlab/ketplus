#pragma once

#include <QProcess>

namespace ketplus::daemon {
// Provider CLIs start helpers of their own (shell wrappers, MCP servers, node workers).
// On Windows QProcess::kill() ends only the root process, so every provider process is
// placed in a Job Object: killing the tree ends the helpers too, and closing the job (the
// daemon exiting or the QProcess being destroyed) ends whatever is left. Other platforms keep
// the plain QProcess behaviour, so these functions are safe to call everywhere.

// Tie `process` to a private job. Call once, right after construction; the process is
// assigned to the job as soon as it starts, and the remaining members are ended when the root
// exits. The window between CreateProcess and assignment is not covered: a grandchild spawned
// in those first microseconds escapes (documented in docs/daemon-architecture.md).
void attachProcessTree(QProcess* process);

// Ends `process` and, on Windows, everything started inside its job.
void killProcessTree(QProcess* process);

// Makes the daemon itself a member of a kill-on-close job, so every child it ever started is
// ended when the daemon dies, including when it is killed. No-op off Windows.
bool confineDaemonProcessTree(QString* error = nullptr);
} // namespace ketplus::daemon
