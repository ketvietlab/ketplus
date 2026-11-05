#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>

namespace ketplus::daemon {
// Runtime-local catalogs: shared by conversations, never persisted with credentials.
class ProviderCatalogCache final : public QObject {
  public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;
    using Fetch = std::function<void(Callback)>;
    explicit ProviderCatalogCache(QObject* parent = nullptr, int freshMs = 60000,
                                  int timeoutMs = 20000, int retryMs = 10000);
    void get(const QString& provider, Fetch fetch, Callback done);
    void invalidate(const QString& provider);

  private:
    struct Entry {
        QJsonObject value;
        QString error;
        qint64 fetchedAt{0};
        qint64 attemptedAt{0};
        quint64 flight{0};
        bool fetching{false};
        QList<Callback> waiters;
    };
    void finish(const QString& provider, quint64 flight, const QJsonObject& value,
                const QString& error);
    QHash<QString, Entry> entries_;
    quint64 nextFlight_{0};
    QElapsedTimer clock_;
    int freshMs_, timeoutMs_, retryMs_;
};
} // namespace ketplus::daemon
