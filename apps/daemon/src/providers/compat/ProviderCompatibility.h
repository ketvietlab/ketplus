#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace ketplus::daemon::compat {

// Provider CLI compatibility (ADR 0015): the supported range of each user-installed CLI comes
// from provider-compatibility.json; a detected version maps to one support status.
enum class SupportStatus {
    Supported, // within [minimum, newest tested]
    Newer,     // above the newest tested version: runs, with a warning
    Outdated,  // below the minimum: the provider is not started
    Unknown,   // the version could not be read: runs, with a warning
    Missing,   // no executable was found
    Bundled,   // shipped inside KetPlus (Claude), so always the tested version
};

[[nodiscard]] QString statusName(SupportStatus status);

// Dotted numeric version: "codex-cli 0.154.0" -> 0.154.0, "2026.09.18-9a7762b" -> 2026.9.18.
struct Version final {
    QList<int> parts;
    [[nodiscard]] bool isValid() const noexcept { return !parts.isEmpty(); }
    [[nodiscard]] QString toString() const;
};
[[nodiscard]] Version parseVersion(const QString& text);
// Missing trailing parts count as zero, so 1.2 == 1.2.0.
[[nodiscard]] int compare(const Version& left, const Version& right);

// A range of CLI versions that share one wire format and a baseline capability set. A dialect
// covers versions from `from` up to the next dialect's `from`.
struct Dialect final {
    QString id;
    Version from;
    QStringList capabilities;
};

struct Policy final {
    QString provider;
    QString name;
    Version minimum;
    Version newestTested;
    QStringList tested;
    QString updateCommand;
    QString homebrewUpdateCommand;
    // Ascending by `from`.
    QList<Dialect> dialects;
    // Bundled runtimes (Claude) have no version range, only capabilities.
    bool bundled{false};
    QStringList capabilities;
    [[nodiscard]] bool isValid() const noexcept { return minimum.isValid(); }
    // Homebrew installs are updated through brew, everything else through the default command.
    [[nodiscard]] QString updateCommandFor(const QString& executable) const;
    // The dialect for `version`; an unreadable version uses the newest dialect. Null when the
    // version is below every dialect or none is declared.
    [[nodiscard]] const Dialect* dialectFor(const Version& version) const;
    // Before a version probe answers, assume the newest dialect (or the bundled list). A provider
    // without any declared capabilities is not gated.
    [[nodiscard]] bool baselineSupports(const QString& capability) const;
};
// Empty (invalid) policy for providers without a manifest entry.
[[nodiscard]] Policy policy(const QString& provider);
// Tests: evaluate against a manifest document instead of the built-in one.
[[nodiscard]] Policy policyFrom(const QJsonObject& manifest, const QString& provider);

struct Runtime final {
    QString provider;
    QString name;
    QString executable;
    QString version;
    SupportStatus status{SupportStatus::Unknown};
    QString minimum;
    QString newestTested;
    QString updateCommand;
    // User-facing explanation; empty when the status needs none.
    QString message;
    // Bundled runtimes list their component versions (for Claude: SDK and Claude Code).
    QJsonObject components;
    // Dialect id and the capabilities of this runtime (ADR 0015 §3), sorted.
    QString dialect;
    QStringList capabilities;
    [[nodiscard]] bool blocksLaunch() const noexcept { return status == SupportStatus::Outdated; }
    [[nodiscard]] bool supports(const QString& capability) const {
        return capabilities.contains(capability);
    }
    [[nodiscard]] QJsonObject toJson() const;
};

// `versionOutput` is what `<cli> --version` printed; empty when the probe failed.
[[nodiscard]] Runtime evaluate(const Policy& policy, const QString& executable,
                               const QString& versionOutput);
[[nodiscard]] Runtime missing(const Policy& policy);
// The Claude sidecar records its SDK version in runtime.json next to index.mjs.
[[nodiscard]] Runtime bundledClaude(const QString& sidecarEntry);
// Tests: the same against a manifest document.
[[nodiscard]] Runtime bundledClaude(const QString& sidecarEntry, const Policy& policy);

} // namespace ketplus::daemon::compat
