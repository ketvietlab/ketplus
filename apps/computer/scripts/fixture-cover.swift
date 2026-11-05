// Occluder for background checks (ADR 0031): a red window from a process without a
// bundle, so its tier is blocked. It covers the given global top-left rectangle
// above the fixture without becoming the frontmost app.
// With "normal" it is an ordinary window, as another app's would be (ADR 0032).
// Run: fixture-cover <x> <y> <width> <height> [normal]
import AppKit

let numbers = CommandLine.arguments.dropFirst().compactMap { Double($0) }
guard numbers.count == 4 else { fatalError("usage: fixture-cover x y width height [normal]") }
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let primaryHeight = NSScreen.screens[0].frame.height
let rect = NSRect(x: numbers[0], y: primaryHeight - numbers[1] - numbers[3], width: numbers[2], height: numbers[3])
let window = NSWindow(contentRect: rect, styleMask: [.borderless], backing: .buffered, defer: false)
window.level = CommandLine.arguments.last == "normal" ? .normal : NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue + 1)
window.backgroundColor = .systemRed
window.orderFrontRegardless()
print("ready")
fflush(stdout)
DispatchQueue.main.asyncAfter(deadline: .now() + 600) { NSApp.terminate(nil) }
app.run()
