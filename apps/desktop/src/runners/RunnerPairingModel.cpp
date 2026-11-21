#include "runners/RunnerPairingModel.h"

#include <QJsonArray>

namespace ketplus::runners {
namespace {

QString text(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString{};
}

PairingView::Stage stage(const QString& state) {
    using Stage = PairingView::Stage;
    if (state == QStringLiteral("ticketed"))
        return Stage::Waiting;
    if (state == QStringLiteral("claimed"))
        return Stage::Confirm;
    if (state == QStringLiteral("approving"))
        return Stage::Approving;
    if (state == QStringLiteral("active"))
        return Stage::Paired;
    if (state == QStringLiteral("rejected"))
        return Stage::Rejected;
    if (state == QStringLiteral("expired"))
        return Stage::Expired;
    if (state == QStringLiteral("ticket_used"))
        return Stage::Used;
    return Stage::Failed;
}

} // namespace

bool PairingView::finished() const {
    return stage != Stage::Waiting && stage != Stage::Confirm && stage != Stage::Approving;
}

std::optional<PairingView> parsePairing(const QJsonObject& result) {
    PairingView view;
    view.ticketId = text(result, QStringLiteral("ticket_id"));
    if (view.ticketId.isEmpty())
        return std::nullopt;
    view.stage = stage(text(result, QStringLiteral("state")));
    const QDateTime expires =
        QDateTime::fromString(text(result, QStringLiteral("expires_at")), Qt::ISODateWithMs);
    if (expires.isValid())
        view.expiresAt = expires.toUTC();
    view.failureReason = text(result, QStringLiteral("failure_reason"));
    view.lastError = text(result, QStringLiteral("last_error"));
    view.runnerId = text(result, QStringLiteral("runner_id"));
    const QJsonObject runner = result.value(QStringLiteral("runner")).toObject();
    if (!runner.isEmpty()) {
        if (const QString id = text(runner, QStringLiteral("runner_id")); !id.isEmpty())
            view.runnerId = id;
        view.runnerName = text(runner, QStringLiteral("name"));
        view.runnerOs = text(runner, QStringLiteral("os"));
        view.runnerArch = text(runner, QStringLiteral("arch"));
        view.fingerprint = text(runner, QStringLiteral("fingerprint"));
        for (const QJsonValue& label : runner.value(QStringLiteral("labels")).toArray()) {
            if (label.isString() && !label.toString().isEmpty())
                view.runnerLabels.append(label.toString());
        }
    }
    // A code is shown only while it can still be compared.
    if (view.stage == PairingView::Stage::Confirm || view.stage == PairingView::Stage::Approving)
        view.confirmationCode = text(result, QStringLiteral("confirmation_code"));
    const QJsonValue attempts = result.value(QStringLiteral("attempts_left"));
    if (attempts.isDouble() && attempts.toInt(-1) >= 0)
        view.attemptsLeft = attempts.toInt();
    return view;
}

QString pairingToken(const QJsonObject& result) { return text(result, QStringLiteral("token")); }

QString nodeCommand(const bool newRunner) {
    return newRunner ? QStringLiteral("ketplus-runner register -")
                     : QStringLiteral("ketplus-runner pair -");
}

} // namespace ketplus::runners
