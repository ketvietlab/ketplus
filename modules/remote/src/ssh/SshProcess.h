#pragma once
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QStringList>

// Internal to SshTunnel's translation units: the options and environment every ssh call shares.
namespace ketplus::remote::ssh {
Q_DECLARE_LOGGING_CATEGORY(lcSsh)

// The flags every call shares: no prompt of any kind, no tty, and none of the user's
// multiplexing, forwarding, remote or local commands (they would change what runs and where).
// Calls that can authenticate log at ERROR, so a server banner never reaches stderr. `stdio`
// leaves stdin open for `ssh -W`.
[[nodiscard]] QStringList batchOptions(int connectTimeout, bool stdio = false,
                                       const QString& logLevel = QStringLiteral("ERROR"));
// ProxyJump hops read the user's config, not these options: no askpass dialog on any hop.
[[nodiscard]] QProcessEnvironment sshEnvironment();
} // namespace ketplus::remote::ssh
