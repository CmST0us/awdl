import XCTest

final class AirDropTests: XCTestCase {
    private let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
    private let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")

    @MainActor func testSendFile() throws {
        continueAfterFailure = false
        let environment = ProcessInfo.processInfo.environment
        let filename = try XCTUnwrap(environment["AIRDROP_FILENAME"])
        let receiver = try XCTUnwrap(environment["AIRDROP_RECEIVER_NAME"])
        let timeout = try XCTUnwrap(environment["AIRDROP_REQUEST_TIMEOUT"].flatMap(Double.init))
        let stepTimeout = try XCTUnwrap(environment["AIRDROP_STEP_TIMEOUT"].flatMap(Double.init))
        let files = XCUIApplication(bundleIdentifier: "com.apple.DocumentsApp")
        let storage = try FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory())
        let available = try XCTUnwrap(storage[.systemFreeSize] as? NSNumber).uint64Value
        FileHandle.standardOutput.write(Data("AIRDROP_UI {\"event\":\"storage\",\"available_bytes\":\(available)}\n".utf8))

        XCUIDevice.shared.orientation = .portrait
        settings.launch()
        try tap(settings, labels: ["Wi-Fi", "无线局域网"], timeout: stepTimeout)
        capture(settings, name: "send-wifi")
        files.launch()
        if files.buttons["完成"].exists { files.buttons["完成"].tap() }
        let search = files.searchFields.firstMatch
        if !search.exists {
            try tap(files, labels: ["Search", "搜索"], timeout: stepTimeout)
        }
        XCTAssertTrue(search.waitForExistence(timeout: stepTimeout))
        search.tap()
        if let value = search.value as? String, !value.isEmpty, value != search.placeholderValue {
            search.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: value.count))
        }
        search.typeText(filename)
        let matches = files.cells.matching(NSPredicate(format: "identifier BEGINSWITH %@", filename + ","))
        XCTAssertTrue(matches.firstMatch.waitForExistence(timeout: timeout), "没有找到待发送文件：\(filename)")
        // Files keeps the underlying directory in its accessibility tree;
        // the search result is the last matching cell above that directory.
        let file = try XCTUnwrap(matches.allElementsBoundByIndex.last)
        capture(files, name: "send-file-selected")
        files.buttons["OverflowBarButtonItem"].tap()
        try tap(files, labels: ["Select", "选择"], timeout: stepTimeout)
        file.coordinate(withNormalizedOffset: CGVector(dx: 0.4, dy: 0.5)).tap()
        let share = files.buttons.matching(NSPredicate(format: "label IN %@", ["Share", "共享"]))
        XCTAssertTrue(share.firstMatch.waitForExistence(timeout: stepTimeout))
        try XCTUnwrap(share.allElementsBoundByIndex.last).tap()
        capture(files, name: "send-share-sheet")
        try tap(files, labels: ["AirDrop", "隔空投送"], timeout: stepTimeout)
        event("send-ready")
        let target = files.buttons.matching(NSPredicate(format: "label CONTAINS %@", receiver)).firstMatch
        XCTAssertTrue(target.waitForExistence(timeout: timeout), "AirDrop 未发现接收端：\(receiver)")
        capture(files, name: "send-target")
        target.tap()
        event("send-requested")
        let completed = files.staticTexts.matching(NSPredicate(format: "label IN %@", ["Sent", "已发送"])).firstMatch
        XCTAssertTrue(completed.waitForExistence(timeout: timeout), "AirDrop 未确认发送完成")
        event("sent")
        capture(files, name: "send-completed")
    }

    @MainActor func testReceiveFile() throws {
        continueAfterFailure = false
        let environment = ProcessInfo.processInfo.environment
        let sender = try XCTUnwrap(environment["AIRDROP_SENDER_NAME"])
        let filename = try XCTUnwrap(environment["AIRDROP_FILENAME"])
        let timeout = try XCTUnwrap(environment["AIRDROP_REQUEST_TIMEOUT"].flatMap(Double.init))
        let stepTimeout = try XCTUnwrap(environment["AIRDROP_STEP_TIMEOUT"].flatMap(Double.init))

        let storage = try FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory())
        let available = try XCTUnwrap(storage[.systemFreeSize] as? NSNumber).uint64Value
        FileHandle.standardOutput.write(Data("AIRDROP_UI {\"event\":\"storage\",\"available_bytes\":\(available),\"thermal_state\":\(ProcessInfo.processInfo.thermalState.rawValue)}\n".utf8))

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
