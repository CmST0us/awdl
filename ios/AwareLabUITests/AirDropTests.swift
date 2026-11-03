import XCTest

final class AirDropTests: XCTestCase {
    private let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
    private let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")

    @MainActor func testReceiveFile() throws {
        continueAfterFailure = false
        let environment = ProcessInfo.processInfo.environment
        let sender = try XCTUnwrap(environment["AIRDROP_SENDER_NAME"])
        let filename = try XCTUnwrap(environment["AIRDROP_FILENAME"])
        let timeout = try XCTUnwrap(environment["AIRDROP_REQUEST_TIMEOUT"].flatMap(Double.init))
        let stepTimeout = try XCTUnwrap(environment["AIRDROP_STEP_TIMEOUT"].flatMap(Double.init))

        XCUIDevice.shared.orientation = .portrait
        settings.launch()
        capture(settings, name: "settings")
        try tap(settings, labels: ["General", "通用"], timeout: stepTimeout)
        try tap(settings, labels: ["AirDrop", "隔空投送"], timeout: stepTimeout)
        capture(settings, name: "airdrop-settings")
        try tap(settings, labels: ["Receiving Off", "接收关闭"], timeout: stepTimeout)
        let everyone = settings.buttons.matching(
            NSPredicate(format: "label BEGINSWITH %@ OR label BEGINSWITH %@", "Everyone", "所有人")
        ).firstMatch
        XCTAssertTrue(everyone.waitForExistence(timeout: stepTimeout), "缺少 AirDrop 所有人选项")
        everyone.tap()
        let selected = XCTNSPredicateExpectation(predicate: NSPredicate(format: "selected == true"), object: everyone)
        XCTAssertEqual(XCTWaiter.wait(for: [selected], timeout: stepTimeout), .completed)
        capture(settings, name: "airdrop-everyone")
        XCUIDevice.shared.press(.home)
        event("ready")

        let accept = springboard.buttons.matching(
            NSPredicate(format: "label IN %@", ["Accept", "接受"])
        ).firstMatch
        guard accept.waitForExistence(timeout: timeout) else {
            capture(springboard, name: "airdrop-request-missing")
            XCTFail("没有收到 AirDrop 接收提示")
            return
        }
        capture(springboard, name: "airdrop-request")
        let senderText = springboard.staticTexts.matching(
            NSPredicate(format: "label CONTAINS %@", sender)
        ).firstMatch
        XCTAssertTrue(senderText.exists, "AirDrop 提示的发送方不匹配：\(sender)")
        XCTAssertTrue(senderText.label.contains(filename), "AirDrop 提示的文件名不匹配：\(filename)")
        accept.tap()
        XCTAssertTrue(accept.waitForNonExistence(timeout: stepTimeout), "接受后系统提示没有关闭")
        event("accepted")
        capture(springboard, name: "airdrop-accepted")
    }

    @MainActor private func tap(_ app: XCUIApplication, labels: [String], timeout: Double) throws {
        let element = app.descendants(matching: .any).matching(
            NSPredicate(format: "label IN %@", labels)
        ).firstMatch
        XCTAssertTrue(element.waitForExistence(timeout: timeout), "缺少系统选项：\(labels)")
        element.tap()
    }

    private func event(_ name: String) {
        FileHandle.standardOutput.write(Data("AIRDROP_UI {\"event\":\"\(name)\"}\n".utf8))
    }

    @MainActor private func capture(_ app: XCUIApplication, name: String) {
        let tree = XCTAttachment(string: app.debugDescription)
        tree.name = name + "-hierarchy"
        tree.lifetime = .keepAlways
        add(tree)
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }
}
