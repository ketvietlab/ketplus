#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

#include <optional>

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
// A stream may have gaps: logical ranges with no bytes. A page never joins the bytes on both
// sides of a gap, and a cursor in a gap moves to the next stored byte as truncated.
class BackgroundTaskOutput final {
  public:
    static constexpr qint64 maximumBytes = 10 * 1024 * 1024;
    static constexpr int maximumPageBytes = 8 * 1024;

    explicit BackgroundTaskOutput(QSqlDatabase database);
    bool initialize(QString* error);
    bool append(const QString& taskId, const QByteArray& bytes, QString* error,
                qint64 producerRevision = 0);
    // The logical offset of the stream (bytes and gaps), 0 for a task with none; nothing when
    // it could not be read.
    [[nodiscard]] std::optional<qint64> total(const QString& taskId,
                                              QString* error = nullptr) const;
    // Appends only when the logical offset is `expectedOffset`, checked and written in one
    // transaction; otherwise writes nothing and fails.
    bool appendAt(const QString& taskId, qint64 expectedOffset, const QByteArray& bytes,
                  QString* error);
    // Records that [expectedOffset, toOffset) was lost: the logical offset moves to `toOffset`
    // with no bytes, and the retained tail follows it. Only when the logical offset is
    // `expectedOffset`; `toOffset` equal to it changes nothing, below it fails.
    bool markGap(const QString& taskId, qint64 expectedOffset, qint64 toOffset, QString* error);
    [[nodiscard]] BackgroundOutputPage read(const QString& taskId, const QString& cursor = {},
                                            int limit = maximumPageBytes, bool textBoundary = false,
                                            bool flushTrailing = false) const;

  private:
    bool write(const QString& taskId, const QByteArray& bytes, std::optional<qint64> expected,
               qint64 producerRevision, QString* error);

    QSqlDatabase database_;
};

} // namespace ketplus::daemon
