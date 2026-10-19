import ComputerProtocol
import CoreGraphics
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

struct Screenshot {
    let png: Data
    let geometry: ScreenGeometry
}

enum CaptureError: Error, CustomStringConvertible {
    case displayUnavailable
    case encodeFailed

    var description: String {
        switch self {
        case .displayUnavailable: return "The main display is not available for capture."
        case .encodeFailed: return "The screenshot could not be encoded as PNG."
        }
    }
}

/// Captures the main display, excluding KetPlus windows so the agent never
/// reads its own transcript back from the screen.
enum ScreenCapture {
    static let excludedBundleIdentifiers: Set<String> = ["dev.ketplus.editor", AgentPaths.bundleIdentifier]

    static func captureMainDisplay() async throws -> Screenshot {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        let mainID = CGMainDisplayID()
        guard let display = content.displays.first(where: { $0.displayID == mainID }) else {
            throw CaptureError.displayUnavailable
        }
        let excluded = content.applications.filter { excludedBundleIdentifiers.contains($0.bundleIdentifier) }
        let filter = SCContentFilter(display: display, excludingApplications: excluded, exceptingWindows: [])

        let geometry = ScreenGeometry(displayBounds: CGDisplayBounds(mainID))
        let configuration = SCStreamConfiguration()
        configuration.width = geometry.imageWidth
        configuration.height = geometry.imageHeight
        configuration.showsCursor = true
        configuration.scalesToFit = true

        let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: configuration)
        return Screenshot(png: try encodePNG(image),
                          geometry: ScreenGeometry(displayBounds: geometry.displayBounds,
                                                   imageWidth: image.width, imageHeight: image.height))
    }

    private static func encodePNG(_ image: CGImage) throws -> Data {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else {
            throw CaptureError.encodeFailed
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else { throw CaptureError.encodeFailed }
        return data as Data
    }
}
