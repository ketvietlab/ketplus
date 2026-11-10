#pragma once

#include <QJsonObject>
#include <QVariantMap>

namespace ketplus {

// The setup guide for a provider that cannot work (#170, ADR 0110), built from what the daemon
// reports: a `provider.runtime_status` entry and its `readiness`, or a turn's coded
// `runtime.error`. Settings → Providers, the composer and the transcript show the same guide.
//
// The map has the mock's shape (mockups/qt Fixtures.providerSetup):
//   id, provider, name, title, badge, tone ("danger" | "warning"), blocking, transient,
//   reason, summary, error, source, steps [{text, command, turn}], primary, primaryAction,
//   secondary, secondaryAction, still, ready, rows [[label, value]].
// Actions: "" checks again, "retry", "settings/<page>", "limits", "model", "switch:<provider>",
// "new:<provider>". Commands are shown, copied or typed into a terminal; KetPlus never runs them.
// An empty map means the provider is ready, or its state needs no guide.
namespace provider_setup {

// `provider` is a runtime_status entry. `turnError`, when given, is the payload of the
// runtime.error that ended a turn ({code, message}); `model` names the turn's model.
[[nodiscard]] QVariantMap guide(const QJsonObject& provider, const QJsonObject& turnError = {},
                                const QString& model = {});
// The guide id for a readiness state or error code, as Fixtures.providerSetup names it
// ("cli-missing", "signed-out", "expired", "limit", ...); empty when there is none.
[[nodiscard]] QString guideId(const QString& provider, const QString& stateOrCode);
// "Nov 13 at 07:00" in English, for a reset given in seconds since the epoch.
[[nodiscard]] QString resetText(qint64 secondsSinceEpoch);

} // namespace provider_setup
} // namespace ketplus
