#pragma once

#include "providers/compat/ProviderCompatibility.h"

#include <QJsonObject>
#include <QSet>
#include <QString>

namespace ketplus::daemon::compat {

// Capability names (ADR 0015 §3). Adapter and Desktop code ask for these, never for versions.
namespace capability {
inline constexpr QLatin1StringView images{"images"};
inline constexpr QLatin1StringView modelList{"modelList"};
inline constexpr QLatin1StringView rateLimits{"rateLimits"};
inline constexpr QLatin1StringView threadCompact{"threadCompact"};
inline constexpr QLatin1StringView threadFork{"threadFork"};
inline constexpr QLatin1StringView threadRename{"threadRename"};
inline constexpr QLatin1StringView threadResume{"threadResume"};
inline constexpr QLatin1StringView turnInterrupt{"turnInterrupt"};
inline constexpr QLatin1StringView turnSteer{"turnSteer"};
} // namespace capability

// The capability a provider method provides; empty for methods without one.
[[nodiscard]] QString capabilityForMethod(QStringView method);

// Capabilities a running CLI turned out not to have: advertised as absent at initialize, or a
// call answered "method not found" (-32601). They stay removed for the life of the process.
class CapabilityNarrowing final {
  public:
    void remove(const QString& capability);
    // Records a -32601 answer to `method`. Returns the capability that was removed, if any.
    QString methodNotFound(QStringView method);
    // ACP `initialize` result: only capabilities the agent explicitly reports as false are
    // removed, since ACP omits most fields when they keep their default.
    void acpInitialized(const QJsonObject& result);
    void reset() { removed_.clear(); }
    [[nodiscard]] Runtime apply(Runtime runtime) const;
    [[nodiscard]] bool removed(const QString& capability) const {
        return removed_.contains(capability);
    }

  private:
    QSet<QString> removed_;
};

} // namespace ketplus::daemon::compat
