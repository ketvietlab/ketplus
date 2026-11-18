#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Local checks of what the runner sends the Relay, so a value the contract refuses never
// leaves the node.
namespace ketplus::runner::app {

inline constexpr qsizetype maxNameCharacters = 64;
inline constexpr qsizetype maxLabels = 32;
inline constexpr qsizetype maxLabelCharacters = 64;
inline constexpr char runnerProtocolCapability[] = "runner.protocol/1";

// Ids the Relay hands out: 1 to 128 of [A-Za-z0-9_-]. Also what makes a machine id a safe
// file name.
[[nodiscard]] bool isRelayId(const QString& text);

// Surrounding whitespace removed; 1 to 64 code points without control characters
// (U+0000-U+001F, U+007F). Nothing when the result breaks a rule.
[[nodiscard]] std::optional<QString> normalizedName(const QString& name);
// The host name cut to 64 code points, as the default name.
[[nodiscard]] QString defaultName(const QString& hostName);

// At most 32 labels, unique, each 1 to 64 characters of ^[a-z0-9][a-z0-9._-]*$. The Relay
// never normalizes a label, so neither does the runner. Empty when valid, else the reason.
[[nodiscard]] QString labelsError(const QStringList& labels);

} // namespace ketplus::runner::app
