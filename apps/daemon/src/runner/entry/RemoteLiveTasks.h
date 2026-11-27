#pragma once

#include "storage/RemoteJobRows.h"

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace ketplus::daemon::runner::entry {

// Reads again every live task whose remote job has started, through `changed`: the remote jobs
// service may have ended one before the runtime held its port, for example at a daemon start.
// Writes nothing; `changed` only reads the task and publishes it.
template <typename LiveTasks, typename Changed>
void readStartedAgain(const LiveTasks& live, const QSqlDatabase& database, const Changed& changed) {
    const QStringList ids = live.keys(); // `changed` may drop a task from `live`
    for (const QString& id : ids)
        if (remote_job_rows::phaseOf(database, id) == remote_job_rows::Phase::Started)
            changed(id);
}

} // namespace ketplus::daemon::runner::entry
