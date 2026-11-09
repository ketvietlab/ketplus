#include "providers/ClaudeCredentials.h"
#include "providers/ClaudeAccountUsage.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>

namespace ketplus::daemon {
namespace {
bool validKey(const QString& key) {
    return !key.startsWith("sk-ant-admin01-") && key.size() <= 512 &&
           QRegularExpression(
               QRegularExpression::anchoredPattern(QStringLiteral("sk-ant-[A-Za-z0-9_-]+")))
               .match(key)
               .hasMatch();
}
} // namespace
ClaudeCredentials::ClaudeCredentials(Storage storage, QObject* parent,
                                     QNetworkAccessManager* network)
    : QObject(parent), storage_(std::move(storage)), network_(network ? network : &ownedNetwork_) {}

QJsonObject ClaudeCredentials::load(QString* error) const {
    error->clear();
    const auto bytes = storage_.read(error);
    if (!error->isEmpty() || bytes.isEmpty())
        return {};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        *error =
            QStringLiteral("The stored Claude credential is unreadable. It has not been replaced.");
        return {};
    }
    const auto config = document.object();
    const auto key = config.value("key").toString();
    const double budget = config.value("budget_usd").toDouble();
    if (!config.value("enabled").isBool() || !config.value("revision").isString() ||
        config.value("revision").toString().isEmpty() || !config.value("budget_usd").isDouble() ||
        !std::isfinite(budget) || budget < 0 || budget > 10000 ||
        (config.contains("key") && (!config.value("key").isString() || !validKey(key))) ||
        (config.value("enabled").toBool() && key.isEmpty())) {
        *error =
            QStringLiteral("The stored Claude configuration is invalid. It has not been replaced.");
        return {};
    }
    return config;
}

QJsonObject ClaudeCredentials::status(const QJsonObject& config) const {
    QJsonObject result{{"configured", !config.value("key").toString().isEmpty()},
                       {"enabled", config.value("enabled").toBool()},
                       {"budget_usd", config.value("budget_usd").toDouble()},
                       {"storage", storage_.name},
                       {"supported", storage_.name != QStringLiteral("unavailable")}};
    result.insert("observation", observation_);
    return result;
}

QJsonObject ClaudeCredentials::launchConfiguration(QString* error) const {
    auto config = load(error);
    // This object is passed only over the daemon-owned sidecar's private stdin.
    if (!error->isEmpty())
        return {};
    if (!config.value("enabled").toBool())
        config.remove("key");
    return config;
}

void ClaudeCredentials::observe(const QJsonObject& payload) {
    const auto source = payload.value("source").toString();
    if (source == "ANTHROPIC_API_KEY" || source == "apiKeyHelper" ||
        source == "/login managed key" || source == "none")
        observation_.insert("source", source);
    const auto cost = payload.value("estimated_cost_usd");
    if (cost.isDouble() && std::isfinite(cost.toDouble()) && cost.toDouble() >= 0)
        observation_.insert("estimated_cost_usd", cost);
}

void ClaudeCredentials::handle(const QString& operation, const QJsonObject& payload, Reply reply) {
    if (operation.startsWith("analytics.")) {
        if (accountUsage_)
            accountUsage_->handle(operation.mid(10), payload, std::move(reply));
        else
            reply({}, QStringLiteral("Update the daemon to configure organization analytics."));
        return;
    }
    QString error;
    auto config = load(&error);
    // Removing replaces the entry without its old contents, so an unreadable or invalid entry,
    // which fails every Claude turn closed, can still be cleared from Settings.
    if (!error.isEmpty() && operation != "remove") {
        reply({}, error);
        return;
    }
    if (operation == "status") {
        reply(status(config), {});
        return;
    }
    if (operation == "save" || operation == "remove") {
        if (operation == "remove") {
            config = {{"enabled", false}, {"budget_usd", 0}};
        } else {
            const auto budget = payload.value("budget_usd");
            if ((payload.contains("key") && !payload.value("key").isString()) ||
                !payload.value("enabled").isBool() || !budget.isDouble() ||
                !std::isfinite(budget.toDouble()) || budget.toDouble() < 0 ||
                budget.toDouble() > 10000) {
                reply({}, QStringLiteral(
                              "Choose an authentication mode and a budget from 0 to 10000 USD."));
                return;
            }
            const QString key = payload.value("key").toString();
            if (!key.isEmpty()) {
                if (!validKey(key)) {
                    reply({}, QStringLiteral("Enter a valid Anthropic API key without spaces."));
                    return;
                }
                config.insert("key", key);
            }
            if (payload.value("enabled").toBool() && config.value("key").toString().isEmpty()) {
                reply({}, QStringLiteral("Enter an API key before enabling API billing."));
                return;
            }
            config.insert("enabled", payload.value("enabled"));
            config.insert("budget_usd", budget);
        }
        config.insert("revision", QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!storage_.write(QJsonDocument(config).toJson(QJsonDocument::Compact), &error)) {
            reply({}, error);
            return;
        }
        observation_ = {};
        reply(status(config), {});
        return;
    }
    if (operation != "check") {
        reply({}, QStringLiteral("Unknown credential operation."));
        return;
    }
    const auto key = config.value("key").toString();
    if (key.isEmpty()) {
        reply({}, QStringLiteral("Save an API key before checking the connection."));
        return;
    }
    // Models listing validates access without running a model or spending inference quota.
    QNetworkRequest request(QUrl(QStringLiteral("https://api.anthropic.com/v1/models?limit=1")));
    request.setRawHeader("x-api-key", key.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    auto* response = network_->get(request);
    connect(response, &QNetworkReply::finished, this, [response, reply = std::move(reply)] {
        const int code = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = response->error() == QNetworkReply::NoError && code == 200;
        response->deleteLater();
        // Never forward provider bodies, headers, URLs or echoed keys into IPC/UI.
        if (ok)
            reply({{"valid", true}}, {});
        else
            reply({},
                  code == 401 ? QStringLiteral("Anthropic rejected this API key.")
                  : code == 403
                      ? QStringLiteral("This API key does not have permission to access models.")
                  : code == 429 ? QStringLiteral("Anthropic rate limit reached. Try again later.")
                                : QStringLiteral("Could not verify Anthropic access (HTTP %1). "
                                                 "Check the connection and try again.")
                                      .arg(code));
    });
}
} // namespace ketplus::daemon
