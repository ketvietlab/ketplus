import CoreGraphics

/// How much of a window the windows above it hide (ADR 0028). Apps such as Qt stop
/// repainting a covered window, so a capture of it can show old pixels.
public enum Occlusion {
    /// Covered at least this much, a window counts as hidden.
    public static let hiddenFraction = 0.5

    /// Fraction of `window` under any of `above`, sampled on a 16 x 16 grid.
    public static func coveredFraction(_ window: CGRect, above: [CGRect]) -> Double {
        guard window.width > 0, window.height > 0, !above.isEmpty else { return 0 }
        let steps = 16
        var hidden = 0
        for column in 0..<steps {
            for row in 0..<steps {
                let point = CGPoint(x: window.minX + (Double(column) + 0.5) * window.width / Double(steps),
                                    y: window.minY + (Double(row) + 0.5) * window.height / Double(steps))
                if above.contains(where: { $0.contains(point) }) { hidden += 1 }
            }
        }
        return Double(hidden) / Double(steps * steps)
    }
}
