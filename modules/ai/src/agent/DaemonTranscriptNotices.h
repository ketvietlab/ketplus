#pragma once
#include <QJsonObject>
#include <QVariantMap>
namespace ketplus::agent_message {
// UI projection only; no provider lifecycle or commands are changed here.
QVariantMap daemonEventNotice(const QString& type, const QJsonObject& data, const QString& itemId,
                              const QJsonObject& usage = {});
QString sessionEstimateText(const QJsonObject& usage);
} // namespace ketplus::agent_message
