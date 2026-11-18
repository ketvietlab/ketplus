#pragma once
#include <QString>

class QLocalSocket;

namespace ketplus::remote {
// Windows: replaces the object's DACL with one entry granting full control to the current
// user only, and blocks inheritance from the parent (so a shared parent such as a profile
// folder cannot add entries). On a directory the entry is inherited by everything created
// inside afterwards. Other platforms keep their POSIX mode-bit handling and return true.
bool restrictToCurrentUser(const QString& path, bool directory, QString* error = nullptr);

// Windows: the named-pipe namespace is shared by every account on the machine and a pipe's name is
// predictable, so another user could create the pipe first and wait for a client to hand it the
// daemon credential. True only when the process serving the connected `socket` runs as the same
// user as this one; a client calls it before sending anything. Other platforms authenticate
// through the owner-only socket directory and return true.
bool serverRunsAsCurrentUser(QLocalSocket* socket, QString* error = nullptr);
} // namespace ketplus::remote
