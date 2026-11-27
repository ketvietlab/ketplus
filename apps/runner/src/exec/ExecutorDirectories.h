#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

// The executor's directory of each job, under the runner's state directory: owner-only, named by
// the job's executor task id, holding the job's identity and lease before its supervisor starts.
namespace ketplus::runner::exec {

class ExecutorDirectories {
  public:
    struct Entry {
        QString path; // canonical
        QString taskId;
        QString jobId;
        qint64 attempt = 0;
        QString lease;
    };

    explicit ExecutorDirectories(QString root);

    // A new directory for the job with a new lease. Empty when it exists already or cannot be
    // written.
    [[nodiscard]] std::optional<Entry> create(const QString& jobId, qint64 attempt);
    // Every name in the root, whatever it holds.
    [[nodiscard]] QStringList names() const;
    // The entry named `name`, when it is a job directory whose identity names it.
    [[nodiscard]] std::optional<Entry> read(const QString& name) const;
    // Removes the directory of `entry`.
    bool remove(const Entry& entry) const;

  private:
    [[nodiscard]] QString canonicalRoot() const;

    QString m_root;
};

} // namespace ketplus::runner::exec
