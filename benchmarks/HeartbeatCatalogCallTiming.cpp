#include "HeartbeatCatalogCallTiming.h"

#include "cloud/CloudControlClient.h"

#include <QElapsedTimer>
#include <QJsonArray>

#include <time.h>

using ketplus::daemon::CloudControlClient;

namespace {
thread_local bool timing = false;
thread_local CatalogCallTiming total;

qint64 threadCpuNs() {
    timespec time{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
        qFatal("Cannot read catalog thread CPU time");
    return qint64(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
} // namespace

void beginCatalogCallTiming() {
    total = {};
    timing = true;
}

CatalogCallTiming endCatalogCallTiming() {
    timing = false;
    return total;
}

// Only the benchmark target wraps the daemon's cross-TU call. The wrapper
// always calls the real implementation and returns its original result.
QJsonArray realSnapshot(const CloudControlClient*) asm("__real_" KETPLUS_SNAPSHOT_SYMBOL);
QJsonArray timedSnapshot(const CloudControlClient*) asm("__wrap_" KETPLUS_SNAPSHOT_SYMBOL);

QJsonArray timedSnapshot(const CloudControlClient* client) {
    if (!timing)
        return realSnapshot(client);
    QElapsedTimer wall;
    wall.start();
    const qint64 cpu = threadCpuNs();
    QJsonArray result = realSnapshot(client);
    total.cpuNs += threadCpuNs() - cpu;
    total.wallNs += wall.nsecsElapsed();
    ++total.calls;
    return result;
}
