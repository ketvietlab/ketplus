#pragma once

#include <QJsonObject>
#include <QWidget>

namespace ketplus {

// Read-only presentation of daemon audit records. No route or finding here grants an action.
class TeamDecisionView final : public QWidget {
    Q_OBJECT
  public:
    TeamDecisionView(const QJsonObject& team, const QJsonObject& task, QWidget* parent = nullptr);
    [[nodiscard]] static bool hasDetails(const QJsonObject& task);
    [[nodiscard]] static QString actorName(const QJsonObject& team, const QJsonValue& actor);
    [[nodiscard]] static QString routeLabel(const QJsonObject& decision);
    [[nodiscard]] static QString summary(const QJsonObject& team, const QJsonObject& decision);
    [[nodiscard]] static QString eventText(const QJsonObject& entry);
};

} // namespace ketplus
