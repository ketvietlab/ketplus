#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <functional>

namespace ketplus::daemon {

class ClaudeAccountUsage;

// Local-only provider credentials. Neither configuration commands nor secrets enter the journal.
class ClaudeCredentials final : public QObject {
    Q_OBJECT
  public:
    struct Storage {
        std::function<QByteArray(QString*)> read;
        std::function<bool(const QByteArray&, QString*)> write;
        QString name;
    };
    using Reply = std::function<void(const QJsonObject&, const QString&)>;
    explicit ClaudeCredentials(Storage storage, QObject* parent = nullptr,
                               QNetworkAccessManager* network = nullptr);
    static Storage keychainStorage(const QString& profile);
    void handle(const QString& operation, const QJsonObject& payload, Reply reply);
    QJsonObject launchConfiguration(QString* error) const;
    void setAccountUsage(ClaudeAccountUsage* usage) { accountUsage_ = usage; }
    void observe(const QJsonObject& payload);

  private:
    QJsonObject load(QString* error) const;
    QJsonObject status(const QJsonObject& config) const;
    ClaudeAccountUsage* accountUsage_{nullptr};
    Storage storage_;
    QNetworkAccessManager ownedNetwork_;
    QNetworkAccessManager* network_;
    QJsonObject observation_;
};
} // namespace ketplus::daemon
