// SPDX-License-Identifier: MPL-2.0
// Renders the approved ketPlus brand rasters from the 1024 px mark and KÉT Sans Bold.
//
//   swift render-brand.swift icon <lite|pro> <mark.png> <KETSans-Bold.ttf> <side> <out.png>
//   swift render-brand.swift lockup <lite|pro> <mark.png> <KETSans-Bold.ttf> <size> <light|dark> <out.png>
//
// The geometry is the one of mockups/qt/qml/parts/BrandLockup.qml: the mark is 1.45× the
// wordmark pixel size, the gap is 0.12× the mark, "Plus" and the framed Pro badge use the
// accent-text blue (#1238DB light, #9EB5FF dark). An icon is the mark on the white tile of the
// mock's Brand screen, inside the macOS icon grid (an 824 px tile on a 1024 px canvas). Jay
// decided (2026-11-12) that app icons carry no "Pro": both editions get the same icon, and the
// edition argument of `icon` is accepted only so callers name what they export.
import AppKit
import CoreText

let args = CommandLine.arguments
func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(2)
}
guard args.count >= 7 else { fail("usage: see the header of render-brand.swift") }
let kind = args[1], edition = args[2]
guard edition == "lite" || edition == "pro" else { fail("edition must be lite or pro") }
guard let mark = NSImage(contentsOfFile: args[3]) else { fail("cannot read the mark") }
let fontURL = URL(fileURLWithPath: args[4])
var registerError: Unmanaged<CFError>?
// A failure here is normally "already registered"; ketSansBold() below fails on a real miss.
_ = CTFontManagerRegisterFontsForURL(fontURL as CFURL, .process, &registerError)
func ketSansBold(_ size: CGFloat) -> NSFont {
    guard let font = NSFont(name: "KETSans-Bold", size: size) else { fail("KETSans-Bold not found") }
    return font
}
func rgb(_ hex: UInt32) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
}

func bitmap(_ width: Int, _ height: Int, _ draw: (CGContext) -> Void) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0,
                               bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let cg = NSGraphicsContext(bitmapImageRep: rep)!.cgContext
    // Flip to a top-left origin so the numbers read like the QML; a flipped NSGraphicsContext
    // keeps text and images upright.
    cg.translateBy(x: 0, y: CGFloat(height))
    cg.scaleBy(x: 1, y: -1)
    let context = NSGraphicsContext(cgContext: cg, flipped: true)
    context.imageInterpolation = .high
    NSGraphicsContext.current = context
    draw(cg)
    NSGraphicsContext.restoreGraphicsState()
    return rep
}

func drawMark(in rect: CGRect) {
    mark.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true,
              hints: [.interpolation: NSImageInterpolation.high])
}

func attributed(_ text: String, _ font: NSFont, _ color: NSColor, kern: CGFloat = 0) -> NSAttributedString {
    NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: color, .kern: kern])
}

// The framed "Pro" badge. `size` is the wordmark pixel size it sits next to; `midY` is the
// vertical centre (the wordmark's x-height middle); `fill` is nil for the transparent frame.
func badgeSize(_ size: CGFloat) -> CGSize {
    let font = ketSansBold(size * 0.6)
    let label = attributed("Pro", font, .black).size()
    return CGSize(width: label.width + font.pointSize * 0.7, height: label.height + font.pointSize * 0.24)
}
func drawBadge(x: CGFloat, midY: CGFloat, size: CGFloat, color: NSColor, fill: NSColor?) -> CGRect {
    let font = ketSansBold(size * 0.6)
    let box = badgeSize(size)
    let rect = CGRect(x: x, y: midY - box.height / 2, width: box.width, height: box.height)
    let stroke = 1.5 * size / 17
    let path = NSBezierPath(roundedRect: rect.insetBy(dx: stroke / 2, dy: stroke / 2),
                            xRadius: box.height * 0.25, yRadius: box.height * 0.25)
    if let fill { fill.setFill(); path.fill() }
    color.setStroke()
    path.lineWidth = stroke
    path.stroke()
    let label = attributed("Pro", font, color)
    let labelSize = label.size()
    label.draw(at: CGPoint(x: rect.midX - labelSize.width / 2, y: rect.midY - labelSize.height / 2))
    return rect
}

func write(_ rep: NSBitmapImageRep, _ path: String) {
    do {
        try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
    } catch { fail("cannot write \(path): \(error)") }
}

switch kind {
case "icon":
    guard let side = Int(args[5]), side > 0 else { fail("side must be a positive integer") }
    let scale = CGFloat(side) / 1024
    write(bitmap(side, side) { cg in
        cg.scaleBy(x: scale, y: scale)
        // macOS icon grid: an 824 px tile with a 185 px corner radius, centred on 1024 px,
        // with the system's soft drop shadow below it.
        let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
        let tilePath = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor(white: 0, alpha: 0.28)
        shadow.shadowBlurRadius = 20
        // Shadow offsets ignore the flipped CTM: a negative height falls below the tile.
        shadow.shadowOffset = NSSize(width: 0, height: -10)
        shadow.set()
        rgb(0xFFFFFF).setFill()
        tilePath.fill()
        NSGraphicsContext.restoreGraphicsState()
        rgb(0xD9D9E0).setStroke()
        tilePath.lineWidth = 4
        tilePath.stroke()
        // The mock's 60 pt tile insets the mark by 4 pt on each side.
        let inset = tile.width * 4 / 60
        // Both editions share this icon: the Pro badge belongs to the lockup, not the icon.
        drawMark(in: tile.insetBy(dx: inset, dy: inset))
    }, args[6])
case "lockup":
    guard args.count >= 8, let size = Double(args[5]).map({ CGFloat($0) }), size > 0 else {
        fail("lockup needs <size> <light|dark> <out.png>")
    }
    let dark = args[6] == "dark"
    let text = dark ? rgb(0xF5F7FF) : rgb(0x202024)
    let brand = dark ? rgb(0x9EB5FF) : rgb(0x1238DB)
    let font = ketSansBold(size)
    let kern = -size * 0.015
    let ket = attributed("ket", font, text, kern: kern)
    let plus = attributed("Plus", font, brand, kern: kern)
    let word = NSMutableAttributedString(attributedString: ket)
    word.append(plus)
    let wordSize = word.size()
    let markSide = size * 1.45
    let wordX = markSide * 1.12
    let badge = badgeSize(size)
    let width = wordX + wordSize.width + (edition == "pro" ? size * 0.3 + badge.width : 0)
    let height = max(markSide, wordSize.height, badge.height)
    let pad = ceil(size * 0.1)
    write(bitmap(Int(ceil(width + 2 * pad)), Int(ceil(height + 2 * pad))) { cg in
        cg.translateBy(x: pad, y: pad)
        drawMark(in: CGRect(x: 0, y: (height - markSide) / 2, width: markSide, height: markSide))
        let wordY = (height - wordSize.height) / 2
        word.draw(at: CGPoint(x: wordX, y: wordY))
        if edition == "pro" {
            // Flipped context: the baseline is the ascender below the line's top.
            let baseline = wordY + font.ascender
            _ = drawBadge(x: wordX + wordSize.width + size * 0.3, midY: baseline - font.xHeight / 2,
                          size: size, color: brand, fill: nil)
        }
    }, args[7])
default:
    fail("unknown kind \(kind)")
}
