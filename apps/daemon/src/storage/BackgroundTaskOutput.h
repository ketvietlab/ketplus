#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

namespace ketplus::daemon {

struct BackgroundOutputPage final {
    QByteArray bytes;
    QString nextCursor;
    qint64 totalBytes{0};
    bool truncated{false};
    QString error;
};

// Uses the journal's connection. The caller authorizes task ownership before reading.
// Cursors carry raw byte positions; text decoding must not change those positions.
class BackgroundTaskOutput final {
  public:
    static constexpr qint64 maximumBytes = 10 * 1024 * 1024;
    static constexpr int maximumPageBytes = 8 * 1024;

    explicit BackgroundTaskOutput(QSqlDatabase database);
    bool initialize(QString* error);
    bool append(const QString& taskId, const QByteArray& bytes, QString* error,
                qint64 producerRevision = 0);
    [[nodiscard]] BackgroundOutputPage read(const QString& taskId, const QString& cursor = {},
                                            int limit = maximumPageBytes, bool textBoundary = false,
                                            bool flushTrailing = false) const;

  private:
    QSqlDatabase database_;
};

} // namespace ketplus::daemon
