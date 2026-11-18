#include "runners/RunnerMessages.h"

#include <QCoreApplication>

namespace ketplus::runners {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("SettingsRunnersPage", text); }

const QString signInAgain = QStringLiteral("account_changed");

} // namespace

QString counted(const int count, const char* one, const char* many) {
    return count == 1 ? tr(one) : tr(many).arg(count);
}

QString plain(const QString& text, const int maximum) {
    QString line;
    line.reserve(text.size());
    for (const QChar character : text)
        line.append(character.category() == QChar::Other_Control ||
                            character == QChar::LineSeparator ||
                            character == QChar::ParagraphSeparator
                        ? QChar(u' ')
                        : character);
    line = line.simplified();
    if (line.size() > maximum)
        line = line.left(maximum - 1) + QChar(0x2026);
    return line;
}

QString unavailableTitle(const Unavailable unavailable) {
    switch (unavailable) {
    case Unavailable::NotEnrolled:
        return tr("Sign in to use runners");
    case Unavailable::RelayUnavailable:
        return tr("Runners are not available");
    case Unavailable::NoEntitlement:
        return tr("Runners are not included");
    case Unavailable::DaemonUnsupported:
        return tr("Update KetPlus to use runners");
    case Unavailable::DaemonUnreachable:
        return tr("KetPlus Daemon is not running");
    case Unavailable::Network:
        return tr("Cannot reach KetPlus Relay");
    case Unavailable::None:
        break;
    }
    return {};
}

QString unavailableMessage(const Unavailable unavailable) {
    switch (unavailable) {
    case Unavailable::NotEnrolled:
        return tr("Runners belong to your KetPlus account. Sign in on this computer in Account & "
                  "devices, then come back here.");
    case Unavailable::RelayUnavailable:
        return tr("KetPlus Relay does not offer runners to this computer right now. Refresh "
                  "later, or check your account in Account & devices.");
    case Unavailable::NoEntitlement:
        return tr("Your KetPlus account does not include runners.");
    case Unavailable::DaemonUnsupported:
        return tr("The KetPlus Daemon on this computer has no runner support. Update KetPlus, "
                  "then refresh.");
    case Unavailable::DaemonUnreachable:
        return tr("Runners are managed through the KetPlus Daemon. Start KetPlus again, then "
                  "refresh.");
    case Unavailable::Network:
        return tr("Check the network connection, then refresh.");
    case Unavailable::None:
        break;
    }
    return {};
}

QString relationText(const Relation relation) {
    switch (relation) {
    case Relation::Trusted:
        return tr("Trusted");
    case Relation::NotPaired:
        return tr("Not paired");
    case Relation::PinnedOnly:
    case Relation::PairedOnly:
    case Relation::Disputed:
        return tr("Pairing mismatch");
    }
    return {};
}

QString relationWarning(const Relation relation) {
    switch (relation) {
    case Relation::PinnedOnly:
        return tr("This computer trusts this runner, but KetPlus Relay no longer pairs them. "
                  "Jobs do not go to it. Pair it again, or unpair it.");
    case Relation::PairedOnly:
        return tr("KetPlus Relay pairs this runner with this computer, but this computer does "
                  "not hold its key. Jobs do not go to it. Pair it again, or unpair it.");
    case Relation::Disputed:
        return tr("KetPlus Daemon does not count this runner as trusted. Jobs do not go to it. "
                  "Pair it again, or unpair it.");
    case Relation::Trusted:
    case Relation::NotPaired:
        break;
    }
    return {};
}

QString stateText(const QString& state) {
    if (state == QStringLiteral("idle"))
        return tr("Idle");
    if (state == QStringLiteral("busy"))
        return tr("Busy");
    if (state == QStringLiteral("draining"))
        return tr("Draining");
    if (state == QStringLiteral("offline"))
        return tr("Offline");
    if (state == QStringLiteral("disabled"))
        return tr("Disabled");
    if (state == QStringLiteral("removed"))
        return tr("Removed");
    if (state.isEmpty())
        return tr("Unknown");
    return plain(state, 24);
}

QString lastSeenText(const std::optional<QDateTime>& seen, const QDateTime& now) {
    if (!seen)
        return tr("Never seen");
    const qint64 seconds = qMax<qint64>(0, seen->secsTo(now));
    if (seconds < 60)
        return tr("Seen just now");
    if (seconds < 3600)
        return counted(int(seconds / 60), "Seen 1 minute ago", "Seen %1 minutes ago");
    if (seconds < 86400)
        return counted(int(seconds / 3600), "Seen 1 hour ago", "Seen %1 hours ago");
    return counted(int(seconds / 86400), "Seen 1 day ago", "Seen %1 days ago");
}

QString errorText(const QString& code, const QString& daemonMessage) {
    if (code == QStringLiteral("not_enrolled"))
        return unavailableMessage(Unavailable::NotEnrolled);
    if (code == QStringLiteral("runner_unavailable"))
        return unavailableMessage(Unavailable::RelayUnavailable);
    if (code == QStringLiteral("entitlement_required"))
        return unavailableMessage(Unavailable::NoEntitlement);
    if (code == QStringLiteral("network"))
        return tr("Cannot reach KetPlus Relay. Check the network connection and try again.");
    if (code == signInAgain)
        return tr("The account on this computer changed. Refresh and try again.");
    if (code == QStringLiteral("too_many_pairings"))
        return tr("Too many pairings are open. Finish or cancel one, then try again.");
    if (code == QStringLiteral("runner_not_found"))
        return tr("This runner is no longer in your account. Refresh the list.");
    if (code == QStringLiteral("runner_removed"))
        return tr("This runner was removed from your account. Refresh the list.");
    if (code == QStringLiteral("pin_store_failed"))
        return tr("This computer could not update the runner's stored key. Check that KetPlus "
                  "can write its data, then try again.");
    if (code == QStringLiteral("rate_limited"))
        return tr("Too many requests. Wait a moment, then try again.");
    if (code == QStringLiteral("invalid_response") || code == QStringLiteral("relay_error"))
        return tr("KetPlus Relay sent an unexpected answer. Try again.");
    if (code == QStringLiteral("route_refused") || code == QStringLiteral("invalid_access"))
        return tr("KetPlus Relay refused this computer. Sign in again in Account & devices.");
    const QString detail = plain(daemonMessage, 200);
    return detail.isEmpty() ? tr("The request failed. Try again.")
                            : tr("The request failed: %1").arg(detail);
}

QString pairingErrorText(const QString& code, const std::optional<int> attemptsLeft,
                         const QString& daemonMessage) {
    if (code == QStringLiteral("code_mismatch")) {
        if (attemptsLeft)
            return counted(*attemptsLeft, "The confirmation code did not match. 1 attempt left.",
                           "The confirmation code did not match. %1 attempts left.");
        return tr("The confirmation code did not match.");
    }
    if (code == QStringLiteral("claim_changed"))
        return tr("Another node claimed this token. Reject this pairing and start again.");
    if (code == QStringLiteral("not_claimed"))
        return tr("The node has not claimed the token yet. Run the command on the node first.");
    if (code == QStringLiteral("ticket_expired"))
        return tr("The token expired. Start again.");
    if (code == QStringLiteral("ticket_used"))
        return tr("This token was already used. Start again.");
    if (code == QStringLiteral("ticket_not_found"))
        return tr("This pairing is no longer known to KetPlus Daemon. Start again.");
    if (code == QStringLiteral("pairing_failed") || code == QStringLiteral("signing_failed") ||
        code == QStringLiteral("digest_mismatch"))
        return tr("Pairing failed. Start again.");
    if (code == QStringLiteral("desktop_key_changed"))
        return tr("This computer's key changed during pairing. Start again.");
    return errorText(code, daemonMessage);
}

QString stageText(const PairingView& view) {
    using Stage = PairingView::Stage;
    switch (view.stage) {
    case Stage::Waiting:
        return tr("Waiting for the node to claim the token.");
    case Stage::Confirm:
        return tr("Check that the node shows the same code, then approve. If the codes differ, "
                  "reject.");
    case Stage::Approving:
        return tr("Approving…");
    case Stage::Paired:
        return tr("The runner is paired with this computer. Jobs can go to it.");
    case Stage::Rejected:
        return tr("The pairing was rejected. Nothing was paired.");
    case Stage::Expired:
        return tr("The token expired before the pairing finished. Start again.");
    case Stage::Used:
        return tr("This token was already used. Start again.");
    case Stage::Failed:
        break;
    }
    if (view.failureReason == signInAgain)
        return tr("The account on this computer changed. Start again.");
    if (view.failureReason == QStringLiteral("runner_unavailable"))
        return tr("KetPlus Relay stopped offering runners. Start again later.");
    const QString& reason = view.failureReason;
    if (reason == QStringLiteral("claim_changed"))
        return tr("Another node claimed this token. Start again.");
    if (reason == QStringLiteral("too_many_attempts"))
        return tr("Too many wrong confirmation codes. Nothing was paired. Start again with a new "
                  "token.");
    if (reason == QStringLiteral("invalid_claim"))
        return tr("The node's claim could not be verified. Nothing was paired. Start again.");
    if (reason == QStringLiteral("unexpected_state"))
        return tr("KetPlus Relay reported an unexpected pairing state. Start again.");
    if (reason == QStringLiteral("digest_mismatch"))
        return tr("The node and this computer saw different pairing details. Nothing was paired. "
                  "Start again.");
    if (reason == QStringLiteral("ticket_not_found"))
        return tr("KetPlus Relay no longer knows this token. Start again.");
    if (reason == QStringLiteral("desktop_key_changed"))
        return tr("This computer's key changed during pairing. Start again.");
    if (reason == QStringLiteral("pin_store_failed"))
        return tr("KetPlus Relay paired the runner, but this computer could not store its key, so "
                  "it is listed as untrusted. Check that KetPlus can write its data, then remove "
                  "the runner and pair it again.");
    if (reason == QStringLiteral("unsupported"))
        return tr("KetPlus Relay stopped supporting pairing. Start again later.");
    if (reason == QStringLiteral("not_enrolled"))
        return tr("This computer has no relay credential. Sign in again in Account & devices, "
                  "then start again.");
    if (reason == QStringLiteral("entitlement_required"))
        return tr("This account's plan does not include runners. Nothing was paired.");
    if (reason == QStringLiteral("invalid_access"))
        return tr("KetPlus Relay refused this computer's credential. Sign in again in Account & "
                  "devices, then start again.");
    if (reason == QStringLiteral("route_refused"))
        return tr("KetPlus Daemon refused to send the pairing request. Start again.");
    if (reason == QStringLiteral("invalid_request"))
        return tr("KetPlus Relay refused the pairing request. Start again.");
    return tr("Pairing failed. Start again.");
}

} // namespace ketplus::runners
