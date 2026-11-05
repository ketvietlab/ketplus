#include "providers/ClaudeAccountUsage.h"
#include "runtime/SessionHost.h"
#include <QPointer>

namespace ketplus::daemon {
void SessionHost::readClaudeAccountUsage(const QJsonObject& request, const QString& clientId,
                                         CommandCallback done) {
    // Reuse the existing paired, encrypted configuration read. Validate generation,
    // lifecycle and worktree context before touching any host account information.
    auto result = handleCommand(request, clientId);
    if (!result.value("ok").toBool() || result.value("adapter") != "claude") {
        done(result);
        return;
    }
    if (!claudeAccountUsage_) {
        result.insert("claude_account_usage", QJsonObject{{"supported", false}});
        done(result);
        return;
    }
    QPointer<SessionHost> guard(this);
    // The caller cannot select a credential operation or pass a key. Only refresh,
    // with an empty payload, is reachable. Cache/rate bounds live in the service.
    claudeAccountUsage_->handle(
        "refresh", {},
        [guard, request, clientId, done = std::move(done)](const QJsonObject& status,
                                                           const QString& error) {
            if (!guard)
                return;
            // A deletion, provider switch or context change while fetching must not
            // return the report to a stale conversation scope.
            auto current = guard->handleCommand(request, clientId);
            if (!current.value("ok").toBool() || current.value("adapter") != "claude") {
                done(current);
                return;
            }
            QJsonObject safe;
            if (!error.isEmpty()) {
                safe = {{"error", "Organization usage could not be read. Check the Admin key on "
                                  "the host and retry."}};
            } else {
                for (const auto* field : {"configured", "supported", "stale"})
                    safe.insert(QLatin1String(field), status.value(QLatin1String(field)).toBool());
                QJsonObject report;
                const auto source = status.value("report").toObject();
                for (const auto* field : {"costUsd", "totalTokens", "startingAt", "endingAt",
                                          "dataRefreshedAt", "fetchedAt"}) {
                    if (source.contains(QLatin1String(field)))
                        report.insert(QLatin1String(field), source.value(QLatin1String(field)));
                }
                safe.insert("report", report);
            }
            current.insert("claude_account_usage", safe);
            done(current);
        });
}
} // namespace ketplus::daemon
