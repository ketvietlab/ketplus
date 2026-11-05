#pragma once

#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include <QVariantMap>

namespace ketplus {
class CodexAppServer;

class CodexRateLimits final : public QObject {
    Q_OBJECT
public:
    explicit CodexRateLimits(CodexAppServer* server, QObject* parent = nullptr);
    QVariantMap weeklyLimit() const { return state_; }
    void refresh();
    static QVariantMap parseWeeklyLimit(const QJsonObject& payload);
signals:
    void weeklyLimitChanged(const QVariantMap& state);
private:
    void publish(const QVariantMap& state);
    void invalidate();
    CodexAppServer* server_;
    QVariantMap state_;
    QTimer poll_;
    QTimer timeout_;
    QTimer reset_;
    quint64 generation_{0};
    bool enabled_{false};
    bool pending_{false};
    bool supported_{true};
};
}
