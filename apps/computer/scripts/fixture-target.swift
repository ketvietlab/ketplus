// Click target for the ADR 0026 spike: a floating window with one large
// button. Prints its button frame in global top-left points, then writes
// `clicked` to the given file when the button is pressed and exits.
// Usage: swift fixture-target.swift /tmp/kpc-clicked.txt
import AppKit

let output = CommandLine.arguments.dropFirst().first ?? "/tmp/kpc-clicked.txt"
try? FileManager.default.removeItem(atPath: output)

final class Target: NSObject {
    @objc func pressed(_ sender: NSButton) {
        FileManager.default.createFile(atPath: output, contents: Data("clicked\n".utf8))
        sender.title = "Clicked ✓"
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { NSApp.terminate(nil) }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let target = Target()
let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 360, height: 200),
                      styleMask: [.titled], backing: .buffered, defer: false)
window.title = "KetPlus Computer spike target"
window.level = .floating
window.center()
let button = NSButton(title: "Click me", target: target, action: #selector(Target.pressed(_:)))
button.bezelStyle = .regularSquare
button.font = .systemFont(ofSize: 28, weight: .semibold)
button.frame = NSRect(x: 20, y: 20, width: 320, height: 140)
window.contentView?.addSubview(button)
window.orderFrontRegardless()

// Cocoa uses a bottom-left origin on the primary screen; CGEvent uses top-left.
let onScreen = window.convertToScreen(button.convert(button.bounds, to: nil))
let primaryHeight = NSScreen.screens[0].frame.height
print("button \(Int(onScreen.minX)) \(Int(primaryHeight - onScreen.maxY)) \(Int(onScreen.width)) \(Int(onScreen.height))"
      + " screen \(Int(NSScreen.screens[0].frame.width)) \(Int(primaryHeight))")
fflush(stdout)
DispatchQueue.main.asyncAfter(deadline: .now() + 600) { NSApp.terminate(nil) }
app.run()
