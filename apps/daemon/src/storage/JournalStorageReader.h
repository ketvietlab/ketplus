#pragma once
#include <QJsonObject>
#include <QString>
#include <functional>
#include <optional>
// Whole-journal reads for Settings → Storage (ADR 0108). Each call opens its own read-only
// connection, so it can run on a worker thread while the daemon keeps writing through WAL.
namespace ketplus::daemon::journal_reader {
// The size by part (conversation history, generated images, activity index, free space), the
// largest conversations and the oldest replay row. Nullopt with *error on a failed read.
std::optional<QJsonObject> measure(const QString& databasePath, QString* error);
// Writes one JSON file per conversation into a new folder inside directory and returns
// {"directory": …, "exported": N}. progress(done, total) runs on the calling thread and
// returns false to stop; the files written so far stay.
std::optional<QJsonObject> exportConversations(const QString& databasePath,
                                               const QString& directory,
                                               const std::function<bool(int, int)>& progress,
                                               QString* error);
} // namespace ketplus::daemon::journal_reader
