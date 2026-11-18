#pragma once

#include "pairing/PairingPins.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon {
class CredentialStore;
}

// The runners this machine trusts, in the daemon's credential store beside the device pins:
// account "runner-pins" holds {"version": 1, "runners": {runnerId: DesktopRunnerPin}}.
// A pin is the only source of trust in a runner; the Relay's word is not.
namespace ketplus::daemon::runner {

inline constexpr auto runnerPinsAccount = "runner-pins";

class RunnerPinStore final {
  public:
    explicit RunnerPinStore(CredentialStore& store) : store_(store) {}

    struct Loaded {
        QHash<QString, ketplus::runner::pairing::DesktopRunnerPin> pins;
        // Runner ids whose record did not read back, was another version or named another id.
        QStringList ignored;
        // The whole record could not be read or parsed; no pin is trusted.
        QString error;
    };

    [[nodiscard]] Loaded load() const;
    [[nodiscard]] std::optional<ketplus::runner::pairing::DesktopRunnerPin>
    pin(const QString& runnerId) const;
    // Replaces any pin of the same runner. Records that do not read back are kept as they are.
    bool store(const ketplus::runner::pairing::DesktopRunnerPin& pin, QString* error = nullptr);
    // True when the runner has no pin afterwards, also when it had none.
    bool remove(const QString& runnerId, QString* error = nullptr);

  private:
    CredentialStore& store_;
};

} // namespace ketplus::daemon::runner
