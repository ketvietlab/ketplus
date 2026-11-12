#pragma once

#include <QString>

// Edits of the KetPlus-owned known_hosts file (SshTunnel::defaultKnownHostsFile()), where
// SshTunnel records the host keys a user accepted. The user's own known_hosts files are never
// edited: a stale key there is theirs to remove with `ssh-keygen -R`.
namespace ketplus::remote::ssh {

// True when the file holds a host key line for `knownHostsName` ("host", "[host]:port" or a
// HostKeyAlias), plain or hashed (|1|salt|hash). @revoked and @cert-authority lines do not count.
[[nodiscard]] bool hasKnownHost(const QString& file, const QString& knownHostsName);

// Removes `knownHostsName` from every host key line of the file, as `ssh-keygen -R` does: a line
// that lists only that name goes, a line that lists other names too keeps them, and a hashed line
// for that name goes. Comments, other hosts and @revoked / @cert-authority lines stay. The file
// is rewritten atomically and owner-only; a file that changes during the rewrite (ssh appending a
// key it accepted) is read again. Returns the number of lines changed (0 when the file
// does not exist or names no such host), or -1 with `error` set when it cannot be rewritten.
int removeKnownHost(const QString& file, const QString& knownHostsName, QString* error = nullptr);

} // namespace ketplus::remote::ssh
