#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::agent_message {

// The bold heading a provider puts at the start of its latest reasoning summary, as Codex does
// ("**Profiling the provider handshake**"), with its first letter lowercased for the running
// line: "profiling the provider handshake". Only the heading is read; the summary's body and
// the reasoning content never leave the item. Empty when the item has no such heading.
[[nodiscard]] QString reasoningHeading(const QJsonObject& item);

// "Working · <detail>" while a turn works and a heading is known, otherwise `status` unchanged.
[[nodiscard]] QString workingLine(const QString& status, const QString& detail);

} // namespace ketplus::agent_message
