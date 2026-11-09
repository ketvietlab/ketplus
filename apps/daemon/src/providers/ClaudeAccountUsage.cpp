#include "providers/ClaudeAccountUsage.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrlQuery>
#include <cmath>
#include <memory>

namespace ketplus::daemon {
namespace {
bool validAdminKey(const QString& key) {
    static const QRegularExpression pattern(QStringLiteral("\\Ask-ant-admin01-[A-Za-z0-9_-]+\\z"));
    return key.size() <= 512 && pattern.match(key).hasMatch();
}
QString failure(int code) {
    if (code == 401)
        return QStringLiteral("Anthropic rejected the Admin API key.");
    if (code == 403)
        return QStringLiteral("This Admin key cannot read organization analytics.");
    if (code == 429)
        return QStringLiteral("Anthropic analytics rate limit reached. Try later.");
    return QStringLiteral("Organization analytics could not be refreshed (HTTP %1). Try again.")
        .arg(code);
}
} // namespace
ClaudeAccountUsage::ClaudeAccountUsage(ClaudeCredentials::Storage storage, QObject* parent,
                                       QNetworkAccessManager* network)
    : QObject(parent), storage_(std::move(storage)), network_(network ? network : &ownedNetwork_) {}
QJsonObject ClaudeAccountUsage::load(QString* error) const {
    error->clear();
    const auto bytes = storage_.read(error);
    if (!error->isEmpty() || bytes.isEmpty())
        return {};
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
        (doc.object().contains("key") && !validAdminKey(doc.object().value("key").toString()))) {
        *error =
            QStringLiteral("The stored Admin credential is unreadable. It has not been replaced.");
        return {};
    }
    return doc.object();
}
QJsonObject ClaudeAccountUsage::status(const QJsonObject& config) const {
    const auto now = QDateTime::currentDateTimeUtc();
    const auto age = fetchedAt_.secsTo(now);
    const bool fresh = fetchedAt_.isValid() && age >= 0 && age < 300 &&
                       fetchedAt_.date().month() == now.date().month() &&
                       fetchedAt_.date().year() == now.date().year();
    return {{"configured", !config.value("key").toString().isEmpty()},
            {"supported", storage_.name != "unavailable"},
            {"report", report_},
            {"stale", !fresh}};
}
void ClaudeAccountUsage::handle(const QString& operation, const QJsonObject& payload,
                                ClaudeCredentials::Reply reply) {
    QString error;
    auto config = load(&error);
    // Removing replaces the entry without its old contents, so an unreadable one can be cleared.
    if (!error.isEmpty() && operation != "remove") {
        reply({}, error);
        return;
    }
    if (operation == "status") {
        reply(status(config), {});
        return;
    }
    if (operation == "save" || operation == "remove") {
        if (operation == "remove")
            config = {};
        else {
            const auto key = payload.value("key").toString();
            if (!validAdminKey(key)) {
                reply({}, QStringLiteral("Enter an Anthropic Admin API key (sk-ant-admin01-…), not "
                                         "an inference key."));
                return;
            }
            config = {{"key", key}};
        }
        if (!storage_.write(QJsonDocument(config).toJson(QJsonDocument::Compact), &error)) {
            reply({}, error);
            return;
        }
        ++revision_;
        report_ = {};
        fetchedAt_ = {};
        reply(status(config), {});
        return;
    }
    if (operation != "refresh") {
        reply({}, QStringLiteral("Unknown analytics operation."));
        return;
    }
    if (config.value("key").toString().isEmpty()) {
        reply(status(config), {});
        return;
    }
    // Cache and coalesce by refusing overlap; opening popovers cannot flood the Admin API.
    if (!status(config).value("stale").toBool()) {
        reply(status(config), {});
        return;
    }
    if (busy_) {
        reply({}, QStringLiteral("Organization analytics are already refreshing. Try shortly."));
        return;
    }
    refresh(config, std::move(reply));
}
void ClaudeAccountUsage::refresh(const QJsonObject& config, ClaudeCredentials::Reply reply) {
    busy_ = true;
    struct Read {
        QString key, start, end;
        int endpoint{0}, pages{0};
        QSet<QString> cursors;
        double cost{0}, tokens{0};
        QString refreshedAt;
        quint64 revision;
        QElapsedTimer elapsed;
        ClaudeCredentials::Reply reply;
    };
    auto state = std::make_shared<Read>();
    const auto now = QDateTime::currentDateTimeUtc();
    state->elapsed.start();
    state->key = config.value("key").toString();
    state->start =
        QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0), QTimeZone::UTC)
            .toString(Qt::ISODate);
    state->end = now.toString(Qt::ISODate);
    state->revision = revision_;
    state->reply = std::move(reply);
    auto next = std::make_shared<std::function<void(QString)>>();
    // Weak recursion avoids retaining the Admin key after completion or QObject destruction.
    std::weak_ptr<std::function<void(QString)>> weakNext = next;
    *next = [this, state, weakNext](QString page) {
        auto keep = weakNext.lock();
        if (!keep)
            return;
        QUrl url(QStringLiteral("https://api.anthropic.com/v1/organizations/analytics/") +
                 (state->endpoint == 0 ? QStringLiteral("cost") : QStringLiteral("usage")));
        QUrlQuery query;
        query.addQueryItem("starting_at", state->start);
        query.addQueryItem("ending_at", state->end);
        query.addQueryItem("limit", "100");
        if (!page.isEmpty())
            query.addQueryItem("page", page);
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setRawHeader("x-api-key", state->key.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("anthropic-beta", "analytics-usage-cost-2026-08-24");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(10000);
        auto* response = network_->get(request);
        response->setReadBufferSize(1024 * 1024 + 1);
        connect(response, &QIODevice::readyRead, response, [response] {
            if (response->bytesAvailable() > 1024 * 1024)
                response->abort();
        });
        connect(response, &QNetworkReply::finished, this, [this, state, keep, response] {
            response->deleteLater();
            const auto fail = [this, state](const QString& error) {
                busy_ = false;
                state->key.clear();
                state->reply({}, error);
            };
            if (state->elapsed.elapsed() > 20000) {
                fail(QStringLiteral("Organization analytics timed out. Try again."));
                return;
            }
            if (state->revision != revision_) {
                fail(QStringLiteral("Admin key changed. Refresh the new account."));
                return;
            }
            const int code = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code != 200 || response->error() != QNetworkReply::NoError) {
                fail(failure(code));
                return;
            }
            QJsonParseError parse;
            const auto bytes = response->readAll();
            const auto doc = QJsonDocument::fromJson(bytes, &parse);
            const auto body = doc.object();
            const auto invalid = [&] {
                fail(QStringLiteral("Anthropic returned an unsupported analytics response."));
            };
            if (bytes.size() > 1024 * 1024 || parse.error != QJsonParseError::NoError ||
                !doc.isObject() || !body.value("data").isArray() ||
                !body.value("has_more").isBool()) {
                invalid();
                return;
            }
            const QString refreshed = body.value("data_refreshed_at").toString();
            if (QDateTime::fromString(refreshed, Qt::ISODate).isValid() &&
                (state->refreshedAt.isEmpty() || refreshed < state->refreshedAt))
                state->refreshedAt = refreshed;
            for (const auto& value : body.value("data").toArray()) {
                if (!value.isObject()) {
                    invalid();
                    return;
                }
                const auto row = value.toObject();
                if (state->endpoint == 0) {
                    const auto cost = row.value("cost").toObject();
                    const QString amount = cost.value("amount").toString();
                    static const QRegularExpression decimal(
                        QStringLiteral("\\A[0-9]+(?:\\.[0-9]{1,6})?\\z"));
                    bool ok = false;
                    const double cents = amount.toDouble(&ok);
                    if (cost.value("currency").toString() != "USD" ||
                        !decimal.match(amount).hasMatch() || !ok || !std::isfinite(cents) ||
                        cents < 0) {
                        invalid();
                        return;
                    }
                    state->cost += cents / 100.0;
                } else {
                    const auto tokens = row.value("usage").toObject().value("total_tokens");
                    const double count = tokens.toDouble(-1);
                    if (!tokens.isDouble() || !std::isfinite(count) || count < 0 ||
                        std::floor(count) != count) {
                        invalid();
                        return;
                    }
                    state->tokens += count;
                }
            }
            if (!std::isfinite(state->cost) || state->cost > 1e12 ||
                state->tokens > 9007199254740991.0) {
                invalid();
                return;
            }
            if (body.value("has_more").toBool()) {
                const QString cursor = body.value("next_page").toString();
                if (cursor.isEmpty() || cursor.size() > 4096 || state->cursors.contains(cursor) ||
                    ++state->pages > 20) {
                    invalid();
                    return;
                }
                state->cursors.insert(cursor);
                (*keep)(cursor);
                return;
            }
            if (state->endpoint++ == 0) {
                state->pages = 0;
                state->cursors.clear();
                (*keep)({});
                return;
            }
            fetchedAt_ = QDateTime::currentDateTimeUtc();
            report_ = {{"costUsd", state->cost},
                       {"totalTokens", state->tokens},
                       {"startingAt", state->start},
                       {"endingAt", state->end},
                       {"dataRefreshedAt", state->refreshedAt},
                       {"fetchedAt", fetchedAt_.toString(Qt::ISODate)}};
            busy_ = false;
            state->key.clear();
            state->reply(status({{"key", "configured"}}), {});
        });
    };
    (*next)({});
}
} // namespace ketplus::daemon
