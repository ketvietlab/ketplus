import ComputerProtocol
import CoreGraphics
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

struct Screenshot {
    let png: Data
    let geometry: ScreenGeometry
    /// Bundle IDs of the granted apps that had windows in the capture.
    let visibleApps: [String]
}

enum CaptureError: Error, CustomStringConvertible {
    case displayUnavailable
    case nothingGranted
    case nothingOnDisplay
    case encodeFailed

    var description: String {
        switch self {
        case .displayUnavailable: return "The display is not available for capture."
        case .nothingGranted: return "No granted application is running. Call list_apps and request_access first."
        case .nothingOnDisplay:
            return "No granted app has a window on this display. Call open_application to bring one here,"
                + " or pass another display from list_apps."
        case .encodeFailed: return "The screenshot could not be encoded as PNG."
        }
    }
}

/// Captures only the windows of applications granted to the conversation. Every
/// other app, KetPlus, this agent's panels and the menu bar stay out of the image.
enum ScreenCapture {
    /// `region` is display-local points (top-left origin); nil captures the display.
    static func capture(including granted: Set<String>, region: CGRect? = nil,
                        display displayID: CGDirectDisplayID = CGMainDisplayID()) async throws -> Screenshot {
        let content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw CaptureError.displayUnavailable
        }
        let apps = content.applications.filter {
            granted.contains($0.bundleIdentifier) && !AppPolicy.ketPlusBundleIdentifiers.contains($0.bundleIdentifier)
        }
        guard !apps.isEmpty else { throw CaptureError.nothingGranted }
        let bounds = CGDisplayBounds(displayID)
        let visible = Set(content.windows.compactMap { window -> String? in
            guard let id = window.owningApplication?.bundleIdentifier, granted.contains(id),
                  window.frame.intersects(bounds) else { return nil }
            return id
        })
        // ScreenCaptureKit fails with -3811 when the filter has no window on the display.
        guard !visible.isEmpty else { throw CaptureError.nothingOnDisplay }
        let filter = SCContentFilter(display: display, including: apps, exceptingWindows: [])

        let configuration = SCStreamConfiguration()
        configuration.showsCursor = true
        configuration.scalesToFit = true
        let geometry: ScreenGeometry
        if let region {
            configuration.sourceRect = region
            let native = nativeScale(displayID, bounds: bounds)
            let fit = ScreenGeometry(displayBounds: CGRect(origin: .zero, size: CGSize(
                width: region.width * native, height: region.height * native)))
            configuration.width = fit.imageWidth
            configuration.height = fit.imageHeight
            geometry = fit
        } else {
            // Pin the whole display. Left unset, a filter whose only window is a small
            // modal panel was scaled up to fill the image, breaking click mapping.
            configuration.sourceRect = CGRect(origin: .zero, size: bounds.size)
            geometry = ScreenGeometry(displayBounds: bounds)
            configuration.width = geometry.imageWidth
            configuration.height = geometry.imageHeight
        }

        let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: configuration)
        return Screenshot(png: try encodePNG(image),
                          geometry: region == nil
                              ? ScreenGeometry(displayBounds: bounds, imageWidth: image.width, imageHeight: image.height)
                              : geometry,
                          visibleApps: visible.sorted())
    }

    /// Physical pixels per point on the display, so zoom never upsamples.
    private static func nativeScale(_ display: CGDirectDisplayID, bounds: CGRect) -> CGFloat {
        guard let mode = CGDisplayCopyDisplayMode(display), bounds.width > 0 else { return 1 }
        return max(1, CGFloat(mode.pixelWidth) / bounds.width)
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
