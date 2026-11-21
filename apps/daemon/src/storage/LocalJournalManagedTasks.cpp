#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

namespace ketplus::daemon {
BackgroundTaskLedger LocalJournal::backgroundTasks() const {
    return BackgroundTaskLedger(database_);
}

bool LocalJournal::initializeManagedTasks() {
    QString error;
    if (!backgroundTasks().initialize(&error)) {
        setError(error);
        return false;
    }
    return true;
}
} // namespace ketplus::daemon
