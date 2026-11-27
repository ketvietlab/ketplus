#pragma once

#include "runners/RunnerListModel.h"
#include "runners/RunnerPairingModel.h"

#include <QDateTime>
#include <QString>

#include <optional>

// Words shown on Settings → Runners for the daemon's fields and codes. Strings that come from
// the Relay or a runner are passed through plain() before they are shown.
namespace ketplus::runners {

// One line of untrusted text: control characters become spaces, and a long value is cut.
[[nodiscard]] QString plain(const QString& text, int maximum = 80);
// `one` for a count of 1, else `many` with the count for %1; both translated.
[[nodiscard]] QString counted(int count, const char* one, const char* many);

[[nodiscard]] QString unavailableTitle(Unavailable unavailable);
[[nodiscard]] QString unavailableMessage(Unavailable unavailable);

[[nodiscard]] QString relationText(Relation relation);
// Explains a mismatch; empty for the other relations.
[[nodiscard]] QString relationWarning(Relation relation);
// A state the desktop knows in words; any other value as it came, cut.
[[nodiscard]] QString stateText(const QString& state);
[[nodiscard]] QString lastSeenText(const std::optional<QDateTime>& seen, const QDateTime& now);

// A failed runner.list, row action or runner.pairing.create: the code in words, else a general
// sentence with the daemon's own message.
[[nodiscard]] QString errorText(const QString& code, const QString& daemonMessage);
// A failed runner.pairing.approve, reject or status.
[[nodiscard]] QString pairingErrorText(const QString& code, std::optional<int> attemptsLeft,
                                       const QString& daemonMessage);
// What a pairing's stage means for the user, with what to do next.
[[nodiscard]] QString stageText(const PairingView& view);

} // namespace ketplus::runners
