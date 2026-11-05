#include "runtime/ProviderCatalogCache.h"
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <utility>
namespace ketplus::daemon {
ProviderCatalogCache::ProviderCatalogCache(QObject* parent, int freshMs, int timeoutMs, int retryMs)
    : QObject(parent), freshMs_(freshMs), timeoutMs_(timeoutMs), retryMs_(retryMs) {
    clock_.start();
}
void ProviderCatalogCache::get(const QString& key, Fetch fetch, Callback done) {
    auto& entry = entries_[key];
    const auto now = clock_.elapsed();
    // Keep the last successful catalog through transient failures, bounded to 15 minutes.
    const bool usable = !entry.value.isEmpty() && now - entry.fetchedAt < 15 * 60 * 1000;
    const bool fresh = usable && now - entry.fetchedAt < freshMs_;
    const bool backoff =
        !entry.fetching && !entry.error.isEmpty() && now - entry.attemptedAt < retryMs_;
    const bool start = !entry.fetching && !fresh && !backoff;
    QJsonObject cached = entry.value;
    const auto error = entry.error;
    quint64 flight = entry.flight;
    if (start) {
        entry.fetching = true;
        entry.attemptedAt = now;
        entry.flight = flight = ++nextFlight_;
    }
    if (!usable && !backoff)
        entry.waiters.append(std::move(done));
    if (start) {
        const QPointer<ProviderCatalogCache> guard(this);
        QTimer::singleShot(timeoutMs_, this, [guard, key, flight] {
            if (guard)
                guard->finish(key, flight, {}, "Model catalog request timed out.");
        });
        // Queue refresh so a usable cache is returned before touching the provider.
        QTimer::singleShot(0, this, [guard, key, flight, fetch = std::move(fetch)] {
            if (!guard || guard->entries_.value(key).flight != flight)
                return;
            fetch([guard, key, flight](const QJsonObject& value, const QString& failure) {
                if (guard)
                    guard->finish(key, flight, value, failure);
            });
        });
    }
    if (usable)
        done(cached, {});
    else if (backoff)
        done({}, error);
}
void ProviderCatalogCache::finish(const QString& key, quint64 flight, const QJsonObject& value,
                                  const QString& error) {
    auto found = entries_.find(key);
    if (found == entries_.end() || !found->fetching || found->flight != flight)
        return;
    found->fetching = false;
    found->error = error;
    if (error.isEmpty() && value.value("catalog").isArray() &&
        !value.value("catalog").toArray().isEmpty()) {
        found->value = value;
        found->fetchedAt = clock_.elapsed();
    } else if (error.isEmpty())
        found->error = "The provider returned no models.";
    const auto failure = found->error;
    const auto waiters = std::exchange(found->waiters, {});
    for (const auto& done : waiters)
        done(failure.isEmpty() ? value : QJsonObject{}, failure);
}
void ProviderCatalogCache::invalidate(const QString& key) {
    const auto entry = entries_.take(key);
    for (const auto& done : entry.waiters)
        done({}, "Provider runtime changed; refresh settings.");
}
} // namespace ketplus::daemon
