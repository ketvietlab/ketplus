import ComputerProtocol
import CoreGraphics
import Foundation
import XCTest

final class ScreenGeometryTests: XCTestCase {
    func testRetinaDisplayDownscalesToLongEdge() {
        let geometry = ScreenGeometry(displayBounds: CGRect(x: 0, y: 0, width: 1728, height: 1117))
        XCTAssertEqual(geometry.imageWidth, 1372)
        XCTAssertEqual(geometry.imageHeight, 887)
    }

    func testSmallDisplayIsNotUpscaled() {
        let geometry = ScreenGeometry(displayBounds: CGRect(x: 0, y: 0, width: 1024, height: 768))
        XCTAssertEqual(geometry.imageWidth, 1024)
        XCTAssertEqual(geometry.imageHeight, 768)
    }

    func testImagePixelMapsToGlobalPointOnOffsetDisplay() {
        let geometry = ScreenGeometry(displayBounds: CGRect(x: -1920, y: 0, width: 1920, height: 1080),
                                      imageWidth: 960, imageHeight: 540)
        let point = geometry.globalPoint(imageX: 0, imageY: 0)
        XCTAssertEqual(point.x, -1919, accuracy: 0.001)
        XCTAssertEqual(point.y, 1, accuracy: 0.001)
        let last = geometry.globalPoint(imageX: 959, imageY: 539)
        XCTAssertEqual(last.x, -1, accuracy: 0.001)
        XCTAssertEqual(last.y, 1079, accuracy: 0.001)
    }

    func testBoundsRejectPixelsOutsideImage() {
        let geometry = ScreenGeometry(displayBounds: CGRect(x: 0, y: 0, width: 100, height: 50))
        XCTAssertTrue(geometry.contains(imageX: 99, imageY: 49))
        XCTAssertFalse(geometry.contains(imageX: 100, imageY: 0))
        XCTAssertFalse(geometry.contains(imageX: -1, imageY: 0))
    }
}

final class LineSocketTests: XCTestCase {
    func testRoundTripsObjectsAndReportsPeerUser() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("kpc-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("s").path

        let listener = try LineListener(path: path)
        let received = expectation(description: "server echoed")
        Thread.detachNewThread {
            guard let server = try? listener.accept(), let request = try? server.readObject() else { return }
            XCTAssertEqual(server.peerUserID(), getuid())
            try? server.writeObject(["echo": request["value"] ?? NSNull()])
            received.fulfill()
        }
        let client = try LineConnection.connect(to: path)
        try client.writeObject(["value": "xin chào"])
        let reply = try client.readObject()
        wait(for: [received], timeout: 5)
        XCTAssertEqual(reply["echo"] as? String, "xin chào")
    }

    func testSocketIsOwnerOnly() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("kpc-\(UUID().uuidString.prefix(8))", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("s").path
        let listener = try LineListener(path: path)
        _ = listener
        let mode = try FileManager.default.attributesOfItem(atPath: path)[.posixPermissions] as? Int
        XCTAssertEqual((mode ?? 0) & 0o077, 0)
    }
}
