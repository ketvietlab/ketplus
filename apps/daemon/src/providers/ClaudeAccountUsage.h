#pragma once
#include "providers/ClaudeCredentials.h"
#include <QDateTime>
#include <QSet>

namespace ketplus::daemon {
// Separate Keychain entry; never reachable from launchConfiguration or the SDK.
class ClaudeAccountUsage final : public QObject {
    Q_OBJECT
  public:
    ClaudeAccountUsage(ClaudeCredentials::Storage storage, QObject* parent = nullptr,
                       QNetworkAccessManager* network = nullptr);
    void handle(const QString& operation, const QJsonObject& payload,
                ClaudeCredentials::Reply reply);

  private:
    QJsonObject load(QString* error) const;
    QJsonObject status(const QJsonObject& config) const;
    void refresh(const QJsonObject& config, ClaudeCredentials::Reply reply);
    ClaudeCredentials::Storage storage_;
    QNetworkAccessManager ownedNetwork_;
    QNetworkAccessManager* network_;
    QJsonObject report_;
    QDateTime fetchedAt_;
    quint64 revision_{0};
    bool busy_{false};
};
} // namespace ketplus::daemon
