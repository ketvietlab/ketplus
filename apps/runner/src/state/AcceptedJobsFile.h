#pragma once

#include "session/SessionPorts.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

// `accepted.json`: every (jobId, attempt) whose spec this runner accepted in the last 7 days,
//   {"version": 1, "jobs": [{jobId, attempt, acceptedAt}]}
// A record reaches the disk before add() returns true; older records are dropped on load and
// on each add.
namespace ketplus::runner::app {

class StateDirectory;

inline constexpr qint64 acceptedJobsKeepDays = 7;

class AcceptedJobsFile final : public session::AcceptedJobs {
  public:
    // The records on disk, or nothing with `error` set when the file cannot be read or is
    // corrupt (left as it is). A missing file is empty.
    [[nodiscard]] static std::optional<AcceptedJobsFile>
    load(const StateDirectory& directory, std::function<QDateTime()> now, QString* error);

    [[nodiscard]] bool contains(const QString& jobId, qint64 attempt) const override;
    [[nodiscard]] bool add(const QString& jobId, qint64 attempt) override;
    // The reason of the last failed add, naming the file only.
    [[nodiscard]] const QString& error() const { return m_error; }
    [[nodiscard]] qsizetype size() const { return m_jobs.size(); }

  private:
    struct Record {
        QString jobId;
        qint64 attempt = 0;
        QDateTime acceptedAt;
    };
    AcceptedJobsFile(const StateDirectory& directory, std::function<QDateTime()> now)
        : m_directory(&directory), m_now(std::move(now)) {}
    void dropExpired();

    const StateDirectory* m_directory;
    std::function<QDateTime()> m_now;
    QList<Record> m_jobs;
    QString m_error;
};

} // namespace ketplus::runner::app
