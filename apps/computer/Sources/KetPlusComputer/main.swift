import AppKit
import ComputerProtocol

// KetPlus Computer: the only KetPlus process that captures the screen or posts
// input, so TCC attributes both grants to this bundle (ADR 0026).
let application = NSApplication.shared
application.setActivationPolicy(.accessory)

do {
    try AgentServer(paths: .standard()).start()
} catch {
    FileHandle.standardError.write(Data("KetPlus Computer could not start: \(error)\n".utf8))
    exit(1)
}

application.run()
