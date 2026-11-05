@testable import ComputerProtocol
import CoreGraphics
import XCTest

final class WorkDisplayTests: XCTestCase {
    private let main = DisplayInfo(id: 1, name: "Built-in", bounds: CGRect(x: 0, y: 0, width: 1728, height: 1117),
                                   visibleFrame: CGRect(x: 0, y: 33, width: 1728, height: 1010), isMain: true)
    private let side = DisplayInfo(id: 7, name: "Studio", bounds: CGRect(x: 1728, y: -200, width: 2560, height: 1440),
                                   visibleFrame: CGRect(x: 1728, y: -200, width: 2560, height: 1440), isMain: false)

    func testSecondaryIsTheDefaultAndFallsBackToMain() {
        XCTAssertEqual(DisplayPreference(setting: nil), .secondary)
        XCTAssertEqual(DisplayPreference(setting: "bogus"), .secondary)
        XCTAssertEqual(DisplayPreference(setting: "main"), .main)
        XCTAssertEqual(WorkDisplay.choose([main, side], preference: .secondary)?.id, 7)
        XCTAssertEqual(WorkDisplay.choose([side, main], preference: .main)?.id, 1)
        XCTAssertEqual(WorkDisplay.choose([main], preference: .secondary)?.id, 1)
        XCTAssertNil(WorkDisplay.choose([], preference: .secondary))
    }

    func testDisplayArgumentsResolve() {
        let displays = [main, side]
        XCTAssertEqual(WorkDisplay.resolve(nil, displays: displays, work: side)?.id, 7)
        XCTAssertEqual(WorkDisplay.resolve("work", displays: displays, work: side)?.id, 7)
        XCTAssertEqual(WorkDisplay.resolve("main", displays: displays, work: side)?.id, 1)
        XCTAssertEqual(WorkDisplay.resolve("7", displays: displays, work: main)?.id, 7)
        XCTAssertNil(WorkDisplay.resolve("9", displays: displays, work: main))
    }

    func testWindowsMoveInsideTheVisibleFrame() {
        XCTAssertTrue(WorkDisplay.isOn(CGRect(x: 2000, y: 0, width: 400, height: 300), side))
        XCTAssertFalse(WorkDisplay.isOn(CGRect(x: 100, y: 100, width: 400, height: 300), side))
        let placed = WorkDisplay.place(CGRect(x: 100, y: 100, width: 400, height: 300), on: side, index: 1)
        XCTAssertEqual(placed, CGRect(x: 1728 + 68, y: -200 + 68, width: 400, height: 300))
        let huge = WorkDisplay.place(CGRect(x: 0, y: 0, width: 5000, height: 3000), on: main, index: 0)
        XCTAssertEqual(huge, main.visibleFrame)
        XCTAssertTrue(side.visibleFrame.contains(WorkDisplay.place(CGRect(x: 0, y: 0, width: 2500, height: 1400),
                                                                   on: side, index: 7)))
    }
}
