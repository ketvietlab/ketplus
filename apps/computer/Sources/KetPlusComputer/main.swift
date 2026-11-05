import AppKit
import ComputerProtocol

// KetPlus Computer: the only KetPlus process that captures the screen or posts
// input, so TCC attributes both grants to this bundle (ADR 0026).
let application = NSApplication.shared
application.setActivationPolicy(.accessory)

if let flag = CommandLine.arguments.firstIndex(of: "--render-previews"),
   CommandLine.arguments.count > flag + 1 {
    do {
        try MainActor.assumeIsolated {
            try Previews.render(into: URL(fileURLWithPath: CommandLine.arguments[flag + 1]))
        }
        exit(0)
    } catch {
        FileHandle.standardError.write(Data("\(error)\n".utf8))
        exit(1)
    }
}

let paths = AgentPaths.standard()
let tools = MainActor.assumeIsolated {
    AgentTools(paths: paths, control: ControlState(), indicator: ControlIndicator(), cursor: VirtualCursor())
}
do {
    try AgentServer(paths: paths, tools: tools).start()
} catch {
    FileHandle.standardError.write(Data("KetPlus Computer could not start: \(error)\n".utf8))
    exit(1)
}
MainActor.assumeIsolated { tools.installKillSwitch() }
Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
    MainActor.assumeIsolated { tools.expireIdleLease() }
}

application.run()
