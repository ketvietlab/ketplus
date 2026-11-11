#pragma once
#include <QJsonObject>
#include <QVariantMap>
namespace ketplus::agent_message {
// UI projection only; no provider lifecycle or commands are changed here.
// What a turn-end line reports beside its status; a negative value leaves that part out.
struct TurnFacts {
    qint64 durationMs = -1;
    qint64 contextTokens = -1;
    // The user's Stop ended it (`reason: "user"`), as the mock's "stopped by you".
    bool stoppedByUser = false;
};
QVariantMap daemonEventNotice(const QString& type, const QJsonObject& data, const QString& itemId,
                              const QJsonObject& usage = {}, const TurnFacts& turn = {});
QString sessionEstimateText(const QJsonObject& usage);
// "6m 20s · 141k context · $2.08 session", as the mock's turn end reads.
QString turnEndText(const TurnFacts& turn, const QJsonObject& usage);
// The journal events that open a turn, timing its end line.
bool daemonTurnStarts(const QString& type);
// The context a usage.context event reports, or -1.
qint64 daemonContextTokens(const QJsonObject& data);
} // namespace ketplus::agent_message
