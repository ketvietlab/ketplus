#pragma once

#include "providers/compat/ProviderCompatibility.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>
#include <optional>

class QProcess;
class QTimer;

namespace ketplus::daemon::compat {

// Runs `<cli> --version` without blocking the daemon and evaluates it against the manifest.
// Results are cached per executable fingerprint (path, size, modification time), so every
// provider launch re-checks cheaply and a CLI update on disk is noticed at the next launch.
class ProviderVersionProbe final : public QObject {
    Q_OBJECT

  public:
    using Callback = std::function<void(const Runtime&)>;
    struct Invocation final {
        QString program;
        QStringList arguments;
    };
    using InvocationBuilder = std::function<Invocation(const QString& executable)>;

    static constexpr int timeoutMs = 5000;

    explicit ProviderVersionProbe(Policy policy, InvocationBuilder invocation = {},
                                  QObject* parent = nullptr);
    ~ProviderVersionProbe() override;

    // Calls back synchronously when a cached result still matches the executable on disk.
    void check(const QString& executable, Callback callback);
    [[nodiscard]] std::optional<Runtime> cached(const QString& executable) const;
    [[nodiscard]] const Policy& policy() const noexcept { return policy_; }

  private:
    struct Fingerprint final {
        QString path;
        qint64 size{-1};
        QDateTime modified;
        bool operator==(const Fingerprint&) const = default;
    };
    [[nodiscard]] static Fingerprint fingerprintOf(const QString& executable);
    void finish(const QString& output);

    Policy policy_;
    InvocationBuilder invocation_;
    QProcess* process_{nullptr};
    QTimer* timeout_{nullptr};
    QString probing_;
    Fingerprint probingFingerprint_;
    QList<Callback> waiters_;
    std::optional<Fingerprint> cachedFingerprint_;
    Runtime cachedRuntime_;
};

} // namespace ketplus::daemon::compat
