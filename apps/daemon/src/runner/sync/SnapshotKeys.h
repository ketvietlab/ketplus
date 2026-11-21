#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace ketplus::daemon {
class CredentialStore;
}

// The desktop runner secret and the opaque keys derived from it that name a repository and a
// worktree to a runner. No path reaches the runner: only the keys do.
namespace ketplus::daemon::runner::sync {

inline constexpr auto runnerSecretAccount = "runner-snapshot-secret";
inline constexpr int runnerSecretBytes = 32;
inline constexpr int workspaceKeyChars = 32;

// The secret in the daemon's credential store under runnerSecretAccount as
// {"version": 1, "secret": base64url}. It is created on first use only when the account is
// absent; an unreadable or malformed record is an error and is never replaced.
class RunnerSecret final {
  public:
    explicit RunnerSecret(CredentialStore& store) : store_(store) {}
    // The 32 secret bytes, or nullopt with `error` set (the error never holds the secret).
    [[nodiscard]] std::optional<QByteArray> obtain(QString* error = nullptr) const;

  private:
    CredentialStore& store_;
};

// First 32 base64url characters of HMAC-SHA256(secret, canonical([runnerId, repositoryId])).
[[nodiscard]] QString repositoryKey(const QByteArray& secret, const QString& runnerId,
                                    const QString& repositoryId);
// The same over [runnerId, repositoryId, worktreeId].
[[nodiscard]] QString workspaceKey(const QByteArray& secret, const QString& runnerId,
                                   const QString& repositoryId, const QString& worktreeId);

} // namespace ketplus::daemon::runner::sync
