#pragma once

#include <QJsonObject>
#include <QVariantList>

namespace ketplus::agent_message {

struct DaemonDisplayState final {
    QString status;
    bool running;
};
DaemonDisplayState daemonDisplayState(const QString& state);

// Canonical transcript identities shared by the window projection and live
// event handling, so a live echo and its replayed row compare equal.
QString daemonMessageKey(qint64 turnSequence, const QString& itemId);
QString daemonTurnId(qint64 turnSequence);
QString daemonUserItemId(qint64 cursor);
QString daemonNoticeItemId(qint64 cursor);

// Muted transcript note for a `provider.request_declined` daemon event.
QString providerRequestDeclinedText(const QJsonObject& data);
// Note and title for a `provider.protocol_error` event (ADR 0015 §5): a known provider
// message lacked a required field and was skipped.
QString providerProtocolErrorText(const QJsonObject& data);
QString providerProtocolErrorTitle();
// Title of the note for a `provider.runtime` event (an untested or unreadable CLI version).
QString providerRuntimeNoticeTitle();
QVariantMap providerNoticeMessage(const QString& text, const QString& itemId,
                                  const QString& title = QStringLiteral("Request declined"));
// ADR 0027: a user row another conversation's agent wrote (`origin: agent`) shows its sender.
void markAgentOrigin(QVariantMap& entry, const QJsonArray& agentMessages);
// Note and title for an `agent_message.state` event (held, failed or expired message).
QString agentMessageStateText(const QJsonObject& data);
QString agentMessageNoticeTitle();

// Pure presentation projection. Historical events must never change runtime
// lifecycle or submit/resolve approvals.
// Rows are labelled "Codex" by the shared formatter; a row another provider wrote says so.
void labelProvider(QVariantMap& entry, const QString& provider);

QVariantMap daemonActivityGroupMessage(const QJsonObject& summary, qint64 turnSequence);
QVariantList daemonWindowMessages(const QJsonObject& page);
QVariantList mergeDaemonHistory(const QVariantList& older, const QVariantList& newer);

} // namespace ketplus::agent_message
