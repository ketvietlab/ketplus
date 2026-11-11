#pragma once

class QQmlEngine;

namespace ketplus {

// The transcript shows what a model wrote. Rich text there could name a remote image or style
// sheet, and loading it would tell a server that the message was read. The transcript engine
// loads only local resources (files, Qt resources, data URLs); every other request fails
// without touching the network.
void installLocalOnlyNetwork(QQmlEngine* engine);

} // namespace ketplus
