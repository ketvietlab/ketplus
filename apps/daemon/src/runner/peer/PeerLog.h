#pragma once

#include <QLoggingCategory>

// Log lines of the desktop's peer sessions carry only job, runner and connection ids, byte counts
// and codes.
Q_DECLARE_LOGGING_CATEGORY(ketplusRunnerPeer)
