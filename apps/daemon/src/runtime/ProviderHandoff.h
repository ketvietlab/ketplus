#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {

class LocalJournal;

// When a conversation moves to another provider, that provider has not seen the turns another
// provider handled. Instead of replaying them into its context, the daemon writes the
// conversation as a plain transcript and tells the provider where it is; the provider reads as
// much of it as it needs, with its own file tools.
namespace provider_handoff {

// Keep superseded native threads reachable for scoped deletion; never resume them on handoff.
void rememberThread(QJsonObject& threads, const QString& provider, const QString& threadId);
[[nodiscard]] QJsonArray threadReferences(const QString& provider, const QString& threadId,
                                          const QJsonObject& threads);

[[nodiscard]] QString providerName(const QString& provider);

// Renders journal events as the transcript: prompts and answers verbatim, tool activity as short
// summaries. Each turn is headed with its provider and the cursor of its first event.
[[nodiscard]] QString renderTranscript(const QJsonArray& events);

// Durable compacted history of the session generation, independent of client delivery ACKs.
[[nodiscard]] QJsonArray journalEvents(const LocalJournal& journal, const QString& sessionId,
                                       qint64 generation);

// Writes `transcript` to `<directory>/transcript.md`, readable only by this user. Returns the
// file path, or an empty string with `error` set.
[[nodiscard]] QString writeTranscript(const QString& directory, const QString& transcript,
                                      QString* error);

// The note put in front of the first prompt after a switch. `seenCursor` is the last event the
// receiving provider took part in, or 0 when it has never seen this conversation.
[[nodiscard]] QString handoffNote(const QString& fromProvider, const QString& transcriptPath,
                                  qint64 seenCursor);

} // namespace provider_handoff
} // namespace ketplus::daemon
