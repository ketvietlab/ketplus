#pragma once

#include <QString>

namespace ketplus::daemon::runner::entry {

// What the user does about a remote job left in needs_attention.
[[nodiscard]] inline QString remoteManualGuidance() {
    return QStringLiteral("The job may still be running on its runner. Check the runner, stop the "
                          "job there or remove the runner, then acknowledge cleanup in the task "
                          "UI.");
}

} // namespace ketplus::daemon::runner::entry
