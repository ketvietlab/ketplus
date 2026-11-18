#include "exec/NativeExecutor.h"

#include "runtime/executor/NativeJobClient.h"
#include "storage/BackgroundTaskOutput.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QUuid>

#include <algorithm>

namespace ketplus::runner::exec {
namespace {

using daemon::BackgroundTaskOutput;
using daemon::executor::NativeJobClient;

class NativeJob final : public ExecutorJob {
  public:
    NativeJob(const QString& program, const QString& directory, const QString& taskId,
              const QString& lease)
        : m_client(program, directory, taskId, lease) {
        m_client.setRecoveryIdentity({{QStringLiteral("directory"), directory},
                                      {QStringLiteral("executor_lease_identity"), lease}});
    }

    bool start(const QJsonObject& execution, const qint64 timeoutMs, Event event,
               QString* error) override {
        // The job's directory and lease are on disk before this: nothing more to commit.
        return m_client.start(
            execution, timeoutMs, [](const QJsonObject&) { return true; }, std::move(event), error);
    }
    bool reconnect(Event event, QString* error) override {
        return m_client.reconnect(std::move(event), error);
    }
    void stop(const QString& reason) override { m_client.stop(reason); }
    void acknowledge() override { m_client.acknowledge(); }

  private:
    NativeJobClient m_client;
};

// Reads the store read-only, from a byte position of its own.
class NativeSpool final : public OutputSpool {
  public:
    NativeSpool(QString path, QString taskId)
        : m_path(std::move(path)), m_taskId(std::move(taskId)),
          m_connection(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
    ~NativeSpool() override {
        if (m_open)
            QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }

    Page read(const int maxBytes) override {
        Page page;
        if (!m_open) {
            if (!QFileInfo::exists(m_path))
                return page; // nothing written yet
            QSqlDatabase database =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
            database.setDatabaseName(m_path);
            database.setConnectOptions(
                QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=50"));
            if (!database.open()) {
                page.failed = true;
                return page;
            }
            m_open = true;
        }
        const BackgroundTaskOutput store(QSqlDatabase::database(m_connection, false));
        const auto read = store.read(m_taskId, m_cursor,
                                     std::min(maxBytes, BackgroundTaskOutput::maximumPageBytes));
        if (!read.error.isEmpty()) {
            page.failed = true; // tried again at the next read
            return page;
        }
        if (read.truncated) {
            const qint64 floor =
                std::max<qint64>(0, read.totalBytes - BackgroundTaskOutput::maximumBytes);
            page.lost = floor - m_position;
            m_position = floor;
        }
        page.bytes = read.bytes;
        m_position += read.bytes.size();
        m_cursor = read.nextCursor;
        return page;
    }

  private:
    QString m_path;
    QString m_taskId;
    QString m_connection;
    QString m_cursor;
    qint64 m_position = 0;
    bool m_open = false;
};

} // namespace

NativeExecutor::NativeExecutor(QString program) : m_program(std::move(program)) {}

QString NativeExecutor::installedProgram() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/ketplus_job_executor");
}

std::unique_ptr<ExecutorJob> NativeExecutor::job(const QString& directory, const QString& taskId,
                                                 const QString& lease) {
    return std::make_unique<NativeJob>(m_program, directory, taskId, lease);
}

std::unique_ptr<OutputSpool> NativeExecutor::output(const QString& directory,
                                                    const QString& taskId) {
    return std::make_unique<NativeSpool>(directory + QStringLiteral("/output.sqlite"), taskId);
}

} // namespace ketplus::runner::exec
