#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::remote {

// What the conversation's transcript never shows, decided once for the daemon, Desktop and
// the phone (through the cloud projection's `transcript` marker):
// - KetPlus's own agents server (`ketplus_agents`, `mcp__ketplus_agents__*` in Claude's
//   naming): team plumbing such as agents_send, team_* and work_report. What it delivers shows
//   through its own events: agent message cards, decision cards and the work report.
// - `tasks.changed`, the provider's background task snapshot, which the task bar shows.
// Nothing is removed from the journal or the provider's history; only rows are left out.
namespace transcript_visibility {

// The cloud projection marks a hidden event's payload with `transcript: "hidden"`.
inline constexpr auto markerKey = "transcript";
inline constexpr auto hiddenMarker = "hidden";

// A tool call item of KetPlus's agents server: Codex's `server` + `tool`, Claude's
// `mcp__server__tool`, or Cursor's `providerIdentifier` in the call's input.
bool internalTool(const QJsonObject& item);
// An event the transcript leaves out: a background task snapshot, or the lifecycle or output
// of an internal tool call. `payload` is the journaled event payload, with or without `item`.
bool hidden(const QString& eventType, const QJsonObject& payload);

} // namespace transcript_visibility
} // namespace ketplus::remote
