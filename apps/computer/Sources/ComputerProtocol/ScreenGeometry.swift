import CoreGraphics

/// Maps between the downscaled screenshot that the model sees and global
/// display points that CGEvent expects.
public struct ScreenGeometry: Equatable, Sendable {
    /// Long edge of the image sent to the model. Claude Code downscales a
    /// 3456x2234 Retina capture to about 1372x887; the spike uses the same edge.
    public static let maxLongEdge = 1372

    /// Display bounds in global points (top-left origin), from CGDisplayBounds.
    public let displayBounds: CGRect
    public let imageWidth: Int
    public let imageHeight: Int

    public init(displayBounds: CGRect, maxLongEdge: Int = ScreenGeometry.maxLongEdge) {
        self.displayBounds = displayBounds
        let width = max(displayBounds.width, 1)
        let height = max(displayBounds.height, 1)
        // Never upscale: a display smaller than the edge keeps its point size.
        let scale = min(1, Double(maxLongEdge) / Double(max(width, height)))
        imageWidth = max(1, Int((Double(width) * scale).rounded()))
        imageHeight = max(1, Int((Double(height) * scale).rounded()))
    }

    public init(displayBounds: CGRect, imageWidth: Int, imageHeight: Int) {
        self.displayBounds = displayBounds
        self.imageWidth = imageWidth
        self.imageHeight = imageHeight
    }

    public func contains(imageX: Int, imageY: Int) -> Bool {
        imageX >= 0 && imageY >= 0 && imageX < imageWidth && imageY < imageHeight
    }

    /// Global point at the centre of image pixel (x, y).
    public func globalPoint(imageX: Int, imageY: Int) -> CGPoint {
        let sx = Double(displayBounds.width) / Double(imageWidth)
        let sy = Double(displayBounds.height) / Double(imageHeight)
        return CGPoint(x: Double(displayBounds.minX) + (Double(imageX) + 0.5) * sx,
                       y: Double(displayBounds.minY) + (Double(imageY) + 0.5) * sy)
    }
}
