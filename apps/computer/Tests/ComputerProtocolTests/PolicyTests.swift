import ComputerProtocol
import CoreGraphics
import Foundation
import XCTest

final class ConversationTokenTests: XCTestCase {
    // Same vector as tests/ComputerToolLaunchTest.cpp.
    let key = Data(0..<32)

    func testTokenMatchesSharedVector() {
        XCTAssertEqual(ConversationToken.mint(key: key, conversation: "conv-123"),
                       "87a6f0b9a2457797da83b42f05dc619965e1681453effbb6d607042db46b341d")
        XCTAssertEqual(ConversationToken.fileStem("conv-123"), "1723376043e8a9d0090025f147440b99")
    }

    func testVerifyRejectsOtherConversationsAndEmptyInput() {
        let token = ConversationToken.mint(key: key, conversation: "conv-123")
        XCTAssertTrue(ConversationToken.verify(key: key, conversation: "conv-123", token: token))
        XCTAssertFalse(ConversationToken.verify(key: key, conversation: "conv-124", token: token))
        XCTAssertFalse(ConversationToken.verify(key: Data(), conversation: "conv-123", token: token))
        XCTAssertFalse(ConversationToken.verify(key: key, conversation: "", token: token))
        XCTAssertFalse(ConversationToken.verify(key: key, conversation: "conv-123", token: String(token.dropLast())))
    }
}

final class AppPolicyTests: XCTestCase {
    func testTiers() {
        XCTAssertEqual(AppPolicy.tier(for: "dev.ketplus.editor"), .blocked)
        XCTAssertEqual(AppPolicy.tier(for: AgentPaths.bundleIdentifier), .blocked)
        XCTAssertEqual(AppPolicy.tier(for: "com.apple.keychainaccess"), .blocked)
        XCTAssertEqual(AppPolicy.tier(for: ""), .blocked)
        XCTAssertEqual(AppPolicy.tier(for: "com.google.Chrome"), .viewOnly)
        XCTAssertEqual(AppPolicy.tier(for: "com.apple.Safari"), .full)
        XCTAssertEqual(AppPolicy.tier(for: "com.apple.Terminal"), .clickOnly)
        XCTAssertEqual(AppPolicy.tier(for: "com.jetbrains.intellij"), .clickOnly)
        XCTAssertEqual(AppPolicy.tier(for: "com.apple.TextEdit"), .full)
        XCTAssertEqual(AppPolicy.tier(for: "com.apple.TextEdit", denied: ["com.apple.TextEdit"]), .blocked)
    }

    func testActionsPerTier() {
        XCTAssertFalse(AppPolicy.allows(.blocked, .view))
        XCTAssertTrue(AppPolicy.allows(.viewOnly, .view))
        XCTAssertFalse(AppPolicy.allows(.viewOnly, .pointer))
        XCTAssertTrue(AppPolicy.allows(.clickOnly, .pointer))
        XCTAssertFalse(AppPolicy.allows(.clickOnly, .keyboard))
        XCTAssertTrue(AppPolicy.allows(.full, .keyboard))
    }

    func testWarningsForBroadReach() {
        XCTAssertNotNil(AppPolicy.warning(for: "com.apple.finder"))
        XCTAssertNotNil(AppPolicy.warning(for: "com.apple.Terminal"))
        XCTAssertNil(AppPolicy.warning(for: "com.apple.TextEdit"))
    }
}

final class KeyComboTests: XCTestCase {
    func testBackgroundKeyEventsCarryCharacters() {
        XCTAssertEqual(KeyCombo.parse("BackSpace")?.eventCharacters, "\u{7f}")
        XCTAssertEqual(KeyCombo.parse("Left")?.eventCharacters, "\u{F702}")
        XCTAssertEqual(KeyCombo.parse("ctrl+s")?.eventCharacters, "s")
        XCTAssertEqual(KeyCombo.parse("shift+a")?.eventCharacters, "A")
        XCTAssertNil(KeyCombo.parse("F5")?.eventCharacters)
    }

    func testParsesKeysAndCombinations() {
        XCTAssertEqual(KeyCombo.parse("Return"), KeyCombo(keyCode: 36, modifiers: []))
        XCTAssertEqual(KeyCombo.parse("ctrl+s"), KeyCombo(keyCode: 1, modifiers: [.control], character: "S"))
        XCTAssertEqual(KeyCombo.parse("super+shift+Tab"), KeyCombo(keyCode: 48, modifiers: [.command, .shift]))
        XCTAssertEqual(KeyCombo.parse("alt+F4"), KeyCombo(keyCode: 118, modifiers: [.option]))
        XCTAssertEqual(KeyCombo.parse("Page_Down"), KeyCombo(keyCode: 121, modifiers: []))
        XCTAssertEqual(KeyCombo.parse("A"), KeyCombo(keyCode: 0, modifiers: [], character: "A"))
    }

    func testRejectsUnknownOrMalformed() {
        XCTAssertNil(KeyCombo.parse("hyper+a"))
        XCTAssertNil(KeyCombo.parse("ctrl+"))
        XCTAssertNil(KeyCombo.parse("NoSuchKey"))
        XCTAssertNil(KeyCombo.parseModifiers("ctrl+x"))
        XCTAssertEqual(KeyCombo.parseModifiers(""), [])
        XCTAssertEqual(KeyCombo.parseModifiers("ctrl+shift"), [.control, .shift])
    }
}

final class ControlStateTests: XCTestCase {
    func testLeaseBlocksOtherConversationsUntilIdle() throws {
        var clock = Date(timeIntervalSince1970: 1000)
        let state = ControlState(idleTimeout: 90, now: { clock })
        try state.acquire("a")
        XCTAssertThrowsError(try state.acquire("b")) { XCTAssertEqual($0 as? ControlState.Failure, .locked(by: "a")) }
        clock += 89
        XCTAssertThrowsError(try state.acquire("b"))
        try state.acquire("a")
        clock += 90
        XCTAssertTrue(state.expireIdleLease())
        XCTAssertNil(state.activeHolder)
        try state.acquire("b")
        XCTAssertEqual(state.activeHolder, "b")
    }

    func testEscapeRevokesGrantsAndMarksCancelledUntilGrantedAgain() throws {
        let state = ControlState()
        state.grant(["com.apple.TextEdit"], to: "a")
        state.grant(["com.apple.Notes"], to: "b")
        try state.acquire("a")
        XCTAssertEqual(state.cancelActive(), "a")
        XCTAssertTrue(state.grantedApps("a").isEmpty)
        XCTAssertTrue(state.isCancelled("a"))
        XCTAssertEqual(state.grantedApps("b"), ["com.apple.Notes"])
        XCTAssertNil(state.activeHolder)
        state.grant(["com.apple.TextEdit"], to: "a")
        XCTAssertFalse(state.isCancelled("a"))
    }
}

final class GeometryRegionTests: XCTestCase {
    func testRegionAndInverseMapping() {
        let geometry = ScreenGeometry(displayBounds: CGRect(x: 0, y: 0, width: 2000, height: 1000),
                                      imageWidth: 1000, imageHeight: 500)
        XCTAssertEqual(geometry.displayRect(imageRegion: [100, 50, 300, 150]), CGRect(x: 200, y: 100, width: 400, height: 200))
        XCTAssertNil(geometry.displayRect(imageRegion: [300, 50, 100, 150]))
        XCTAssertNil(geometry.displayRect(imageRegion: [0, 0, 1001, 10]))
        let pixel = geometry.imagePoint(global: CGPoint(x: 1999, y: 999))
        XCTAssertEqual(pixel.x, 999)
        XCTAssertEqual(pixel.y, 499)
    }
}
