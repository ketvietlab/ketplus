@testable import ComputerProtocol
import XCTest

final class BackgroundTests: XCTestCase {
    func testClickPressesTheDeepestPressableElement() {
        let path = [AXNodeInfo(role: "AXStaticText"), AXNodeInfo(role: "AXButton", actions: ["AXPress"]),
                    AXNodeInfo(role: "AXGroup", actions: ["AXPress"]), AXNodeInfo(role: "AXWindow")]
        let plan = BackgroundPlanner.click(path: path, rightButton: false)
        XCTAssertEqual(plan?.index, 1)
        XCTAssertEqual(plan?.action, .press)
    }

    func testClickFocusesTextInputAndSelectsRows() {
        XCTAssertEqual(BackgroundPlanner.click(path: [AXNodeInfo(role: "AXTextField", actions: ["AXConfirm"])],
                                               rightButton: false)?.action, .focus)
        XCTAssertEqual(BackgroundPlanner.click(path: [AXNodeInfo(role: "AXStaticText"), AXNodeInfo(role: "AXRow")],
                                               rightButton: false)?.action, .select)
    }

    func testClickStopsAtTheWindowAndNeedsTheRealPointer() {
        let path = [AXNodeInfo(role: "AXGroup"), AXNodeInfo(role: "AXWindow", actions: ["AXRaise"]),
                    AXNodeInfo(role: "AXApplication", actions: ["AXPress"])]
        XCTAssertNil(BackgroundPlanner.click(path: path, rightButton: false))
    }

    func testRightClickShowsTheNearestMenu() {
        let path = [AXNodeInfo(role: "AXButton", actions: ["AXPress"]),
                    AXNodeInfo(role: "AXGroup", actions: ["AXShowMenu"])]
        let plan = BackgroundPlanner.click(path: path, rightButton: true)
        XCTAssertEqual(plan?.index, 1)
        XCTAssertEqual(plan?.action, .showMenu)
        XCTAssertNil(BackgroundPlanner.click(path: [AXNodeInfo(role: "AXButton", actions: ["AXPress"])], rightButton: true))
    }

    func testSecureTextIsRecognised() {
        XCTAssertTrue(AXNodeInfo(role: "AXTextField", subrole: "AXSecureTextField").isSecureText)
        XCTAssertFalse(AXNodeInfo(role: "AXTextField").isSecureText)
    }

    func testReferencesGoStaleAfterANewSnapshotOrInvalidation() throws {
        let table = ReferenceTable<String>()
        let first = table.store(["n1": "submit"], app: "dev.example", conversation: "a")
        let resolved = try table.resolve(snapshot: first, reference: "n1", conversation: "a")
        XCTAssertEqual(resolved.0, "submit")
        XCTAssertEqual(resolved.app, "dev.example")
        XCTAssertThrowsError(try table.resolve(snapshot: first, reference: "n9", conversation: "a")) {
            XCTAssertEqual($0 as? ReferenceTable<String>.Failure, .unknownReference)
        }
        XCTAssertThrowsError(try table.resolve(snapshot: first, reference: "n1", conversation: "b")) {
            XCTAssertEqual($0 as? ReferenceTable<String>.Failure, .stale)
        }
        let second = table.store(["n1": "field"], app: "dev.example", conversation: "a")
        XCTAssertNotEqual(first, second)
        XCTAssertThrowsError(try table.resolve(snapshot: first, reference: "n1", conversation: "a"))
        table.invalidate("a")
        XCTAssertThrowsError(try table.resolve(snapshot: second, reference: "n1", conversation: "a")) {
            XCTAssertEqual($0 as? ReferenceTable<String>.Failure, .stale)
        }
    }

    func testEscapeBelongsToTheUserOutsideControlledApps() {
        XCTAssertTrue(KillSwitchPolicy.consumesEscape(frontmostIsGranted: true, secondsSinceForeground: nil))
        XCTAssertTrue(KillSwitchPolicy.consumesEscape(frontmostIsGranted: false, secondsSinceForeground: 3))
        XCTAssertFalse(KillSwitchPolicy.consumesEscape(frontmostIsGranted: false, secondsSinceForeground: 30))
        XCTAssertFalse(KillSwitchPolicy.consumesEscape(frontmostIsGranted: false, secondsSinceForeground: nil))
    }

    func testKeyboardTargetPrefersExplicitThenLastThenFrontmost() {
        XCTAssertEqual(KeyboardTarget.resolve(explicit: "a", lastTarget: "b", frontmost: "c"), "a")
        XCTAssertEqual(KeyboardTarget.resolve(explicit: nil, lastTarget: "b", frontmost: "c"), "b")
        XCTAssertEqual(KeyboardTarget.resolve(explicit: "", lastTarget: nil, frontmost: "c"), "c")
    }

    func testMenuShortcutsMatchCharacterAndModifiers() throws {
        let selectAll = try XCTUnwrap(KeyCombo.parse("super+a"))
        XCTAssertEqual(selectAll.character, "A")
        XCTAssertTrue(MenuShortcut.matches(itemCharacter: "A", itemModifiers: 0, combo: selectAll))
        XCTAssertFalse(MenuShortcut.matches(itemCharacter: "A", itemModifiers: 1, combo: selectAll))
        XCTAssertFalse(MenuShortcut.matches(itemCharacter: "B", itemModifiers: 0, combo: selectAll))
        let redo = try XCTUnwrap(KeyCombo.parse("super+shift+z"))
        XCTAssertTrue(MenuShortcut.matches(itemCharacter: "Z", itemModifiers: 1, combo: redo))
        let controlOnly = try XCTUnwrap(KeyCombo.parse("ctrl+t"))
        XCTAssertTrue(MenuShortcut.matches(itemCharacter: "T", itemModifiers: 4 | 8, combo: controlOnly))
        XCTAssertNil(try XCTUnwrap(KeyCombo.parse("super+Return")).character)
        XCTAssertFalse(MenuShortcut.matches(itemCharacter: "", itemModifiers: 0, combo: try XCTUnwrap(KeyCombo.parse("Return"))))
    }

    func testScrollStepsClampToTheBar() {
        XCTAssertEqual(ScrollPlan.value(from: 0.5, increasing: true, amount: 2), 0.6, accuracy: 1e-9)
        XCTAssertEqual(ScrollPlan.value(from: 0.1, increasing: false, amount: 5), 0)
        XCTAssertEqual(ScrollPlan.value(from: 0.98, increasing: true, amount: 3), 1)
    }
}
