import XCTest
final class DiscoveryTests: XCTestCase {
    @MainActor func testDiscoveryProbe() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_REPEAT_COUNT"] ?? "1") ?? 1
        XCTAssertTrue((1...10).contains(count))
        for index in 1...count {
            let runID = String(format: "%@-%02d", batchID, index)
            let app = XCUIApplication()
            app.launchEnvironment["AWARE_RUN_ID"] = runID
            app.launchEnvironment["AWARE_EXPERIMENT_ID"] = batchID
            app.launchEnvironment["AWARE_DISCOVERY_TIMEOUT"] = "30"
            app.launch()
            defer { app.terminate() }
            XCTAssertTrue(app.buttons["capability.check"].waitForExistence(timeout: 15))
            XCTAssertTrue(app.staticTexts["run.id"].label.contains(runID))
            app.buttons["capability.check"].tap()
            XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
            XCTAssertEqual(app.staticTexts["session.state"].label, "ready")
            app.buttons["discovery.begin"].tap()
            XCTAssertTrue(app.buttons["discovery.system"].waitForExistence(timeout: 10))
            app.buttons["discovery.system"].tap()
            // These are observed system-owned content, absent from the application's own views.
            XCTAssertTrue(app.staticTexts["未找到设备"].waitForExistence(timeout: 20), app.debugDescription)
            XCTAssertTrue(app.staticTexts["To find devices, ask people to open Aware Lab."].exists)
            XCTAssertTrue(app.buttons["关闭"].exists)
            let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            shot.name = runID + "-system-discovery"; shot.lifetime = .keepAlways; add(shot)
            let tree = XCTAttachment(string: app.debugDescription)
            tree.name = runID + "-system-tree"; tree.lifetime = .keepAlways; add(tree)
            app.buttons["关闭"].tap()
            XCTAssertTrue(app.buttons["discovery.finish"].waitForExistence(timeout: 10))
            XCTAssertFalse(app.staticTexts["未找到设备"].exists)
            app.buttons["discovery.finish"].tap()
            XCTAssertTrue(app.staticTexts["session.state"].waitForExistence(timeout: 10))
            XCTAssertEqual(app.staticTexts["session.state"].label, "cancelled")
            let completed = XCTAttachment(string: "{\"run_id\":\"\(runID)\",\"source\":\"xctest.physical\",\"system_ui_verified\":true,\"returned_to_app\":true,\"monotonic_ns\":\(DispatchTime.now().uptimeNanoseconds)}")
            completed.name = runID + "-ui-result"; completed.lifetime = .keepAlways; add(completed)
            app.terminate()
        }
    }
    @MainActor func testDiscoveryTimeout() throws {
        let app = XCUIApplication()
        app.launchEnvironment["AWARE_RUN_ID"] = "timeout-" + UUID().uuidString
        app.launchEnvironment["AWARE_DISCOVERY_TIMEOUT"] = "2"
        app.launch()
        defer { app.terminate() }
        app.buttons["capability.check"].tap()
        app.buttons["discovery.begin"].tap()
        let predicate = NSPredicate(format: "label == 'timedOut'")
        expectation(for: predicate, evaluatedWith: app.staticTexts["session.state"])
        waitForExpectations(timeout: 10)
        XCTAssertTrue(app.staticTexts["session.error"].exists)
    }
}
