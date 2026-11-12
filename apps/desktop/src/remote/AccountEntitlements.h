#pragma once

#include <QStringList>

#include <optional>

// What the signed-in KetSuite account may use. Paid features are enforced through account
// entitlements, never a build flag (docs/product-architecture.md), so Desktop gates the Pro
// routes on what the relay names for the session and nothing on KETPLUS_EDITION.
namespace ketplus {

// The entitlement that lets an account pair and use Linux machines through the ketPlus Relay.
inline constexpr auto relayEntitlementName = "ketplus.relay";

// What a session that names no entitlements means.
enum class MissingEntitlements {
    // Not entitled: fail closed (D-4). Relay stays off until the relay names the entitlement.
    Deny,
    // Entitled: for a relay that does not send entitlements, and for tests.
    Allow,
};

// D-4: fail closed. The relay names entitlements in its session in a follow-up.
inline constexpr MissingEntitlements missingEntitlements = MissingEntitlements::Deny;

// True when the session's entitlements name the Relay; `entitlements` is nullopt when the
// session named none (see ControlV2Client::entitlements()).
[[nodiscard]] inline bool relayEntitled(const std::optional<QStringList>& entitlements,
                                        const MissingEntitlements missing = missingEntitlements) {
    if (!entitlements)
        return missing == MissingEntitlements::Allow;
    return entitlements->contains(QString::fromLatin1(relayEntitlementName));
}

} // namespace ketplus
