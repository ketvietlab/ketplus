#pragma once

#include <QString>

class QQmlEngine;
class QUrl;

namespace ketplus {

// The transcript shows what a model wrote. Rich text there could name a remote image or style
// sheet, and loading it would tell a server that the message was read. The transcript engine
// loads only local resources (files, Qt resources, data URLs); every other request fails
// without touching the network.
void installLocalOnlyNetwork(QQmlEngine* engine);

// A file path or URL that names another machine: `//host/share/a.png`, `\\host\share\a.png`
// or `file://host/share/a.png`. Opening one starts a connection to that host (SMB on Windows,
// which also offers it the account's credentials), so the transcript treats it as remote.
[[nodiscard]] bool namesAnotherMachine(const QString& path);
[[nodiscard]] bool namesAnotherMachine(const QUrl& url);

// Whether the transcript may open `path` on its own, for a thumbnail or an image's size. A path
// on another machine is opened only inside `worktree`, when the conversation itself works on
// that share.
[[nodiscard]] bool transcriptMayOpen(const QString& path, const QString& worktree = {});

} // namespace ketplus
