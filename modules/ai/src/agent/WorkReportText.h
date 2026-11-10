#pragma once

#include <QString>
#include <QVariantMap>

namespace ketplus::agent_message {

// ADR 0106: a turn's work report as markdown, for Copy conversation and other plain-text
// exports: a heading with the app's counts, then one table row per item.
[[nodiscard]] QString workReportText(const QVariantMap& report);

} // namespace ketplus::agent_message
