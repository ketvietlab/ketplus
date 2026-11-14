#pragma once

// Shared by the agent messaging and turn review tests: a journal, a SessionHost and the
// scripted fake providers in a temporary directory. No real provider is ever started.
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeClaude.h"
#include "FakeCodex.h"
#include "FakeCursor.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>

namespace agent_test {

using namespace ketplus::daemon;

inline QJsonObject request(const QString& type, const QJsonObject& payload = {},
                           const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty())
        result.insert("session_id", sessionId);
    if (generation >= 0)
        result.insert("session_generation", generation);
    return result;
}

inline QString text(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

inline QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

struct Fixture final {
    QTemporaryDir directory;
    QString codexLog;
    QString claudeLog;
    QString cursorLog;
    std::unique_ptr<LocalJournal> journal;
    std::unique_ptr<CodexAdapter> codex;
    std::unique_ptr<ClaudeAdapter> claude;
    std::unique_ptr<CursorAdapter> cursor;
    std::unique_ptr<SessionHost> host;

    bool open(const bool endpoint = false) {
        if (!directory.isValid())
            return false;
        codexLog = directory.filePath("codex.log");
        claudeLog = directory.filePath("claude.log");
        cursorLog = directory.filePath("cursor.log");
        if (!writeFakeCodex(directory.filePath("fake-codex"), codexLog) ||
            !writeFakeClaude(directory.filePath("fake-claude"), claudeLog) ||
            !writeFakeCursor(directory.filePath("fake-cursor"), cursorLog))
            return false;
        journal = std::make_unique<LocalJournal>(directory.filePath("journal.sqlite3"));
        codex = std::make_unique<CodexAdapter>(directory.filePath("fake-codex"));
        claude = std::make_unique<ClaudeAdapter>(directory.filePath("fake-claude"));
        cursor = std::make_unique<CursorAdapter>(directory.filePath("fake-cursor"));
        restart(endpoint);
        return journal->isOpen();
    }

    void restart(const bool endpoint = false) {
        host.reset();
        host =
            std::make_unique<SessionHost>(journal.get(), codex.get(), claude.get(), cursor.get());
        if (endpoint)
            host->setAgentEndpoint(directory.filePath("daemon.sock"), "/opt/ketplus_daemon");
    }

    QString start(const QString& provider, const QString& prompt, const QString& sessionId = {},
                  const QString& worktree = {},
                  const QString& permission = QStringLiteral("workspace-write")) {
        const QJsonObject result = host->handleCommand(
            request("session.start_turn",
                    {{"worktree_path", worktree.isEmpty() ? directory.path() : worktree},
                     {"prompt", prompt},
                     {"options", QJsonObject{{"provider", provider}, {"permission", permission}}}},
                    sessionId, sessionId.isEmpty() ? -1 : 1),
            "desktop-test");
        return result.value("ok").toBool() ? result.value("session_id").toString() : QString{};
    }

    QJsonArray events(const QString& sessionId) const {
        return journal->eventsAfter(sessionId, 1, 0);
    }

    int count(const QString& sessionId, const QString& type) const {
        int total = 0;
        for (const QJsonValue& value : events(sessionId))
            if (value.toObject().value("event_type").toString() == type)
                ++total;
        return total;
    }

    QList<QJsonObject> payloads(const QString& sessionId, const QString& type) const {
        QList<QJsonObject> result;
        for (const QJsonValue& value : events(sessionId))
            if (value.toObject().value("event_type").toString() == type)
                result.append(value.toObject().value("payload").toObject());
        return result;
    }

    QString number(const QString& sessionId) const {
        return journal->conversationNumber(sessionId, 1);
    }

    QJsonObject agents(const QString& sessionId, const QString& operation,
                       const QJsonObject& payload = {}) {
        return host->handleAgentCommand(host->agentToken(sessionId), operation, payload);
    }
};

} // namespace agent_test
