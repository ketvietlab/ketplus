#pragma once
#include <QJsonObject>
#include <QVariantMap>
namespace ketplus::agent_message {
// UI projection only; no provider lifecycle or commands are changed here.
// What a turn-end line reports beside its status; a negative value leaves that part out.
struct TurnFacts {
    qint64 durationMs = -1;
    qint64 contextTokens = -1;
};
QVariantMap daemonEventNotice(const QString& type, const QJsonObject& data, const QString& itemId,
                              const QJsonObject& usage = {}, const TurnFacts& turn = {});
QString sessionEstimateText(const QJsonObject& usage);
// "6m 20s · 141k context · $2.08 session", as the mock's turn end reads.
QString turnEndText(const TurnFacts& turn, const QJsonObject& usage);
} // namespace ketplus::agent_message
