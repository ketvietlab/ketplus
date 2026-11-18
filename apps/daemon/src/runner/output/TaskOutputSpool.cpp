#include "runner/output/TaskOutputSpool.h"

#include "runner/peer/PeerLog.h"

#include <limits>
#include <utility>

namespace ketplus::daemon::runner::output {
namespace {

// A code for the log line; the store's own message stays out of it.
const char* codeOf(const QString& error) {
    return error == QLatin1String("Output offset mismatch.") ? "offset_mismatch" : "storage_error";
}

} // namespace

std::unique_ptr<TaskOutputSpool> TaskOutputSpool::open(QSqlDatabase database,
                                                       const QString& taskId) {
    BackgroundTaskOutput output(database);
    QString error;
    const auto size = output.total(taskId, &error);
    if (!size) {
        qCWarning(ketplusRunnerPeer)
            << "output spool not opened for task" << taskId << "storage_error";
        return nullptr;
    }
    return std::unique_ptr<TaskOutputSpool>(
        new TaskOutputSpool(std::move(database), taskId, *size));
}

TaskOutputSpool::TaskOutputSpool(QSqlDatabase database, QString taskId, const qint64 size)
    : output_(std::move(database)), taskId_(std::move(taskId)), size_(size) {}

bool TaskOutputSpool::append(const QByteArray& bytes) {
    if (bytes.isEmpty())
        return true;
    if (size_ > std::numeric_limits<qint64>::max() - bytes.size())
        return false;
    QString error;
    if (!output_.appendAt(taskId_, size_, bytes, &error)) {
        qCWarning(ketplusRunnerPeer)
            << "output spool append failed for task" << taskId_ << "at" << size_ << codeOf(error);
        return false;
    }
    size_ += bytes.size();
    return true;
}

bool TaskOutputSpool::markGap(const qint64 toOffset) {
    if (toOffset < size_)
        return false;
    QString error;
    if (!output_.markGap(taskId_, size_, toOffset, &error)) {
        qCWarning(ketplusRunnerPeer)
            << "output spool gap failed for task" << taskId_ << "at" << size_ << codeOf(error);
        return false;
    }
    size_ = toOffset;
    return true;
}

} // namespace ketplus::daemon::runner::output
