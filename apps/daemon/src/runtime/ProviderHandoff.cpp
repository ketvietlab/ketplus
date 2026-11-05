#include "runtime/ProviderHandoff.h"

#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

namespace ketplus::daemon::provider_handoff {
namespace {

constexpr qsizetype maximumOutputCharacters = 400;

QString clipped(QString text, const qsizetype limit) {
    text = text.trimmed();
    if (text.size() <= limit)
        return text;
    return text.left(limit) + QStringLiteral("… [%1 more characters]").arg(text.size() - limit);
}

QString indented(const QString& text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines)
        line.prepend(QStringLiteral("    "));
    return lines.join(QLatin1Char('\n'));
}

QString commandText(const QJsonValue& command) {
    if (command.isArray()) {
        QStringList parts;
        for (const QJsonValue& part : command.toArray())
            parts.append(part.toString());
        return parts.join(QLatin1Char(' '));
    }
    return command.toString();
}

// A tool item as one line (and, for commands, a short excerpt of their output).
QString activity(const QJsonObject& item) {
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("commandExecution")) {
        QString line = QStringLiteral("- Ran `%1`")
                           .arg(clipped(commandText(item.value(QStringLiteral("command"))), 300));
        if (item.contains(QStringLiteral("exitCode")) && !item.value("exitCode").isNull())
            line += QStringLiteral(" (exit %1)").arg(item.value(QStringLiteral("exitCode")).toInt());
        const QString output = clipped(item.value(QStringLiteral("aggregatedOutput")).toString(),
                                       maximumOutputCharacters);
        if (!output.isEmpty())
            line += QStringLiteral("\n") + indented(output);
        return line;
    }
    if (type == QStringLiteral("fileChange")) {
        QStringList paths;
        for (const QJsonValue& change : item.value(QStringLiteral("changes")).toArray())
            paths.append(change.toObject().value(QStringLiteral("path")).toString());
        paths.removeAll(QString{});
        return paths.isEmpty() ? QStringLiteral("- Changed files")
                               : QStringLiteral("- Changed %1").arg(paths.join(QStringLiteral(", ")));
    }
    if (type == QStringLiteral("mcpToolCall")) {
        return QStringLiteral("- Used %1 / %2")
            .arg(item.value(QStringLiteral("server")).toString(),
                 item.value(QStringLiteral("tool")).toString());
    }
    if (type == QStringLiteral("webSearch"))
        return QStringLiteral("- Searched the web for \"%1\"")
            .arg(item.value(QStringLiteral("query")).toString());
    if (type.isEmpty() || type == QStringLiteral("reasoning") ||
        type == QStringLiteral("userMessage") || type == QStringLiteral("agentMessage"))
        return {};
    return QStringLiteral("- %1").arg(type);
}

} // namespace

QString providerName(const QString& provider) {
    if (provider == QStringLiteral("claude"))
        return QStringLiteral("Claude");
    if (provider == QStringLiteral("codex") || provider.isEmpty())
        return QStringLiteral("Codex");
    return provider.left(1).toUpper() + provider.mid(1);
}

QString renderTranscript(const QJsonArray& events) {
    QStringList out{QStringLiteral("# Conversation transcript"),
                    QStringLiteral("Written by KetPlus. Each turn is headed with the provider "
                                   "that handled it and the cursor of its first event.")};
    QString provider = QStringLiteral("codex");
    qint64 turn = -1;
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        const QString type = event.value(QStringLiteral("event_type")).toString();
        const QJsonObject data = event.value(QStringLiteral("payload")).toObject();
        const qint64 cursor = event.value(QStringLiteral("cursor")).toInteger();
        if (type == QStringLiteral("session.created")) {
            provider = data.value(QStringLiteral("adapter")).toString(provider);
            continue;
        }
        if (type == QStringLiteral("session.provider_changed")) {
            provider = data.value(QStringLiteral("to")).toString(provider);
            out.append(QStringLiteral("\n---\nSwitched from %1 to %2.")
                           .arg(providerName(data.value(QStringLiteral("from")).toString()),
                                providerName(provider)));
            continue;
        }
        if (type == QStringLiteral("user.message") || type == QStringLiteral("turn.steered")) {
            const qint64 sequence = event.value(QStringLiteral("turn_sequence")).toInteger();
            if (type == QStringLiteral("user.message") && sequence != turn) {
                turn = sequence;
                out.append(QStringLiteral("\n## Turn %1 — %2 [cursor %3]")
                               .arg(sequence)
                               .arg(providerName(provider))
                               .arg(cursor));
            }
            const QString text = data.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty())
                out.append(QStringLiteral("\n**User:**\n%1").arg(text));
            continue;
        }
        if (type == QStringLiteral("item.completed")) {
            QJsonObject item = data.value(QStringLiteral("item")).toObject();
            if (item.isEmpty()) {
                item = data;
                item.insert(QStringLiteral("type"), data.value(QStringLiteral("item_type")));
            }
            if (item.value(QStringLiteral("type")).toString() == QStringLiteral("agentMessage")) {
                const QString text = item.value(QStringLiteral("text")).toString().trimmed();
                if (!text.isEmpty())
                    out.append(QStringLiteral("\n**%1:**\n%2").arg(providerName(provider), text));
                continue;
            }
            const QString line = activity(item);
            if (!line.isEmpty())
                out.append(line);
            continue;
        }
        if (type == QStringLiteral("runtime.error")) {
            out.append(QStringLiteral("- Error: %1")
                           .arg(clipped(data.value(QStringLiteral("message")).toString(), 300)));
            continue;
        }
        if (type == QStringLiteral("turn.completed")) {
            const QString status = data.value(QStringLiteral("status")).toString();
            if (!status.isEmpty() && status != QStringLiteral("completed"))
                out.append(QStringLiteral("- Turn ended: %1").arg(status));
        }
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QJsonArray journalEvents(const LocalJournal& journal, const QString& sessionId,
                         const qint64 generation) {
    QJsonArray events;
    qint64 cursor = 0;
    for (;;) {
        // Raw delivery events are deleted after client ACKs. The compacted projection is
        // the durable conversation history and retains completed text after delta compaction.
        const EventReplayPage page =
            journal.compactedEventsAfter(sessionId, generation, cursor, 500);
        if (page.events.isEmpty())
            break;
        for (const QJsonValue& value : page.events)
            events.append(value);
        // Pages can be short because of the byte budget, and compacted cursors have gaps.
        if (page.nextCursor <= cursor)
            break;
        cursor = page.nextCursor;
    }
    return events;
}

QString writeTranscript(const QString& directory, const QString& transcript, QString* error) {
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("Could not create %1").arg(directory);
        return {};
    }
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
    const QString path = QDir(directory).filePath(QStringLiteral("transcript.md"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(transcript.toUtf8()) < 0 ||
        !file.commit()) {
        if (error)
            *error = file.errorString();
        return {};
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return QFileInfo(path).absoluteFilePath();
}

void rememberThread(QJsonObject& threads, const QString& provider, const QString& threadId) {
    if (threadId.isEmpty())
        return;
    auto entry = threads.value(provider).toObject();
    auto retired = entry.value("retired_thread_ids").toArray();
    const auto previous = entry.value("thread_id").toString();
    if (!previous.isEmpty() && previous != threadId && !retired.contains(previous))
        retired.append(previous);
    entry.insert("retired_thread_ids", retired);
    entry.insert("thread_id", threadId);
    threads.insert(provider, entry);
}

QJsonArray threadReferences(const QString& provider, const QString& threadId,
                            const QJsonObject& threads) {
    QJsonArray result;
    const auto append = [&](const QString& owner, const QString& id) {
        const QJsonObject ref{{"provider", owner}, {"thread_id", id}};
        if (!id.isEmpty() && !result.contains(ref))
            result.append(ref);
    };
    append(provider, threadId);
    for (auto it = threads.begin(); it != threads.end(); ++it) {
        const auto entry = it.value().toObject();
        append(it.key(), entry.value("thread_id").toString());
        for (const auto& id : entry.value("retired_thread_ids").toArray())
            append(it.key(), id.toString());
    }
    return result;
}

QString handoffNote(const QString& fromProvider, const QString& transcriptPath,
                    const qint64 seenCursor) {
    QString note =
        QStringLiteral(
            "[KetPlus handoff] This conversation was being handled by %1 and continues with you. "
            "The transcript of the conversation so far is at:\n%2\n")
            .arg(providerName(fromProvider), transcriptPath);
    note += seenCursor > 0
                ? QStringLiteral("You took part in it earlier; the turns you have not seen are the "
                                 "ones headed with a cursor greater than %1. ")
                      .arg(seenCursor)
                : QStringLiteral("You have not seen any of it. ");
    note += QStringLiteral(
        "Read the end of the transcript first for the current state, and earlier parts only when "
        "you need them. The worktree already contains the changes made so far. Do not mention "
        "this note; answer the user's message below.\n\n");
    return note;
}

} // namespace ketplus::daemon::provider_handoff
