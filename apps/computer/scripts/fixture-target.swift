// Target app for computer-use checks (ADR 0026): a window with a text field and a
// Submit button. It prints both frames in global top-left points, then writes the
// field's text to the output file when Submit is pressed, and exits.
// Build with scripts/build-fixture.sh; run: "<app>/Contents/MacOS/KetPlusComputerFixture" <output>
import AppKit

let output = CommandLine.arguments.dropFirst().first ?? "/tmp/kpc-fixture.txt"
try? FileManager.default.removeItem(atPath: output)

final class Target: NSObject {
    let field = NSTextField(frame: NSRect(x: 20, y: 120, width: 360, height: 28))

    @objc func submit(_ sender: NSButton) {
        FileManager.default.createFile(atPath: output, contents: Data((field.stringValue + "\n").utf8))
        sender.title = "Submitted ✓"
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { NSApp.terminate(nil) }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
// A standard Edit menu, so shortcuts such as super+a behave as in real apps.
let mainMenu = NSMenu()
let editItem = NSMenuItem()
let editMenu = NSMenu(title: "Edit")
editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
editItem.submenu = editMenu
mainMenu.addItem(NSMenuItem())
mainMenu.addItem(editItem)
app.mainMenu = mainMenu
let target = Target()
let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 220),
                      styleMask: [.titled], backing: .buffered, defer: false)
window.title = "KetPlus Computer fixture"
window.level = .floating
// The agent captures the primary display, so the fixture opens there, not on the
// display the user happens to be working on.
let primaryFrame = NSScreen.screens[0].frame
window.setFrameOrigin(NSPoint(x: primaryFrame.midX - window.frame.width / 2, y: primaryFrame.midY))
target.field.font = .systemFont(ofSize: 16)
target.field.placeholderString = "Type here"
let button = NSButton(title: "Submit", target: target, action: #selector(Target.submit(_:)))
button.bezelStyle = .regularSquare
button.font = .systemFont(ofSize: 22, weight: .semibold)
button.frame = NSRect(x: 20, y: 20, width: 360, height: 80)
window.contentView?.addSubview(target.field)
window.contentView?.addSubview(button)
window.makeKeyAndOrderFront(nil)
app.activate(ignoringOtherApps: true)

// Cocoa uses a bottom-left origin on the primary screen; CGEvent uses top-left.
let primaryHeight = NSScreen.screens[0].frame.height
func topLeft(_ view: NSView) -> String {
    let rect = window.convertToScreen(view.convert(view.bounds, to: nil))
    return "\(Int(rect.minX)) \(Int(primaryHeight - rect.maxY)) \(Int(rect.width)) \(Int(rect.height))"
}
print("field \(topLeft(target.field)) button \(topLeft(button)) screen \(Int(NSScreen.screens[0].frame.width)) \(Int(primaryHeight))")
fflush(stdout)
DispatchQueue.main.asyncAfter(deadline: .now() + 600) { NSApp.terminate(nil) }
app.run()
