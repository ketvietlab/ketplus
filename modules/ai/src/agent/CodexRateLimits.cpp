#include "agent/CodexRateLimits.h"
#include "agent/CodexAppServer.h"

#include <QDateTime>
#include <QPointer>
#include <cmath>
#include <limits>

namespace ketplus {
namespace {
QJsonObject codexBucket(const QJsonObject& payload) {
    const auto buckets = payload.value("rateLimitsByLimitId").toObject();
    if (buckets.contains("codex")) return buckets.value("codex").toObject();
    const auto bucket = payload.value("rateLimits").toObject();
    const auto id = bucket.value("limitId").toString();
    return id.isEmpty() || id == "codex" ? bucket : QJsonObject{};
}
}

QVariantMap CodexRateLimits::parseWeeklyLimit(const QJsonObject& payload) {
    const auto bucket = codexBucket(payload);
    for (const auto* key : {"primary", "secondary"}) {
        const auto window = bucket.value(QLatin1String(key)).toObject();
        const auto used = window.value("usedPercent");
        if (window.value("windowDurationMins").toInt() != 10080 || !used.isDouble() ||
            !std::isfinite(used.toDouble()) || used.toDouble() < 0) continue;
        const auto reset = window.value("resetsAt").toInteger();
        if (reset > 0 && reset <= QDateTime::currentSecsSinceEpoch()) continue;
        return {{"available", true}, {"usedPercent", used.toDouble()}, {"resetsAt", reset}};
    }
    return {{"available", false}};
}

CodexRateLimits::CodexRateLimits(CodexAppServer* server, QObject* parent)
    : QObject(parent), server_(server) {
    poll_.setInterval(5 * 60 * 1000);
    timeout_.setSingleShot(true);
    timeout_.setInterval(15000);
    reset_.setSingleShot(true);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (server_->isReady()) refresh();
    });
    connect(&timeout_, &QTimer::timeout, this, [this] { invalidate(); });
    connect(&reset_, &QTimer::timeout, this, [this] {
        invalidate();
        if (server_->isReady()) refresh();
    });
    connect(server_, &CodexAppServer::readyChanged, this, [this](bool ready) {
        if (ready && enabled_) refresh();
        else if (!ready) invalidate();
    });
    connect(server_, &CodexAppServer::serverStopped, this, [this] { invalidate(); });
    connect(server_, &CodexAppServer::serverReloaded, this, [this] { invalidate(); });
    connect(server_, &CodexAppServer::notificationReceived, this,
            [this](const QString& method, const QJsonObject& params) {
        if (method == "account/updated") {
            invalidate();
            const auto auth = params.value("authMode").toString();
            supported_ = auth == "chatgpt" || auth == "chatgptAuthTokens";
            if (enabled_ && supported_) refresh();
        } else if (method == "account/rateLimits/updated" && supported_ &&
                   !codexBucket(params).isEmpty()) {
            ++generation_; // A newer push must win over an in-flight read.
            pending_ = false;
            timeout_.stop();
            publish(parseWeeklyLimit(params));
        }
    });
}

void CodexRateLimits::publish(const QVariantMap& state) {
    reset_.stop();
    state_ = state;
    const auto reset = state.value("resetsAt").toLongLong();
    const auto delay = reset * 1000 - QDateTime::currentMSecsSinceEpoch();
    if (state.value("available").toBool() && delay > 0)
        reset_.start(int(qMin<qint64>(delay, std::numeric_limits<int>::max())));
    emit weeklyLimitChanged(state_);
}

void CodexRateLimits::invalidate() {
    ++generation_;
    pending_ = false;
    timeout_.stop();
    publish({{"available", false}});
}

void CodexRateLimits::refresh() {
    enabled_ = true;
    poll_.start();
    if (pending_ || !supported_) return;
    if (!server_->isAvailable()) { invalidate(); return; }
    pending_ = true;
    const auto generation = ++generation_;
    auto loading = state_;
    loading.insert("loading", true);
    publish(loading);
    timeout_.start();
    server_->call("account/rateLimits/read", {},
                  [self = QPointer<CodexRateLimits>(this), generation]
                  (const QJsonObject& result, const QString& error) {
        if (!self || generation != self->generation_) return;
        self->pending_ = false;
        self->timeout_.stop();
        self->publish(error.isEmpty() ? parseWeeklyLimit(result)
                                     : QVariantMap{{"available", false}});
    });
}
}
