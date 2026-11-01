import XCTest

final class M4DiscoveryTests: XCTestCase {
    private let service = TestConfiguration.service

    /// Opens the system Wi-Fi Aware picker as a subscriber and records what is visible.
    /// A missing publisher is evidence, not a test failure; the picker itself must appear.
    @MainActor func testTargetDiscovery() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_REPEAT_COUNT"] ?? "1") ?? 1
        XCTAssertTrue((1...10).contains(count))
        let target = env["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName
        let approval = env["AWARE_LONG_WAIT_APPROVED"] ?? ""
        let approved = !approval.isEmpty
        let targetTimeout = min(max(Double(env["AWARE_TARGET_TIMEOUT"] ?? "5") ?? 5, 5), 30)

        for index in 1...count {
            let runID = String(format: "%@-%02d", batchID, index)
            let app = XCUIApplication()
            app.launchEnvironment["AWARE_RUN_ID"] = runID
            app.launchEnvironment["AWARE_EXPERIMENT_ID"] = batchID
            app.launchEnvironment["AWARE_MODE"] = "m4"
            app.launchEnvironment["AWARE_TARGET_NAME"] = target
            app.launchEnvironment["AWARE_DISCOVERY_TIMEOUT"] = "\(min(Int(targetTimeout), 30))"
            if approved { app.launchEnvironment["AWARE_LONG_WAIT_APPROVED"] = approval }
            app.launch()
            defer { app.terminate() }

            let capabilityButton = app.descendants(matching: .any)["capability.check"]
            XCTAssertTrue(capabilityButton.waitForExistence(timeout: 5))
            XCTAssertTrue(app.staticTexts["run.id"].label.contains(runID))
            XCTAssertTrue(app.staticTexts["mode.value"].label.contains("m4"))
            XCTAssertTrue(app.staticTexts["m4.target"].label.contains(target))
            app.descendants(matching: .any)["capability.check"].tap()
            XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
            XCTAssertEqual(app.staticTexts["session.state"].label, "ready")

            // The App shows the expected target itself; compare counts before and after the
            // system picker opens so the App's own label cannot be mistaken for a discovery.
            let targetBaseline = matchCount(app, containing: target)
            let serviceBaseline = matchCount(app, containing: service)

            app.buttons["discovery.begin"].tap()
            XCTAssertTrue(app.buttons["discovery.system"].waitForExistence(timeout: 5))
            app.buttons["discovery.system"].tap()

            let pickerShown = app.buttons["关闭"].waitForExistence(timeout: 5)
            let tree = app.debugDescription
            let screenshot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            screenshot.name = runID + "-system-discovery"
            screenshot.lifetime = .keepAlways
            add(screenshot)
            let treeAttachment = XCTAttachment(string: tree)
            treeAttachment.name = runID + "-system-tree"
            treeAttachment.lifetime = .keepAlways
            add(treeAttachment)
            XCTAssertTrue(pickerShown, "System picker was not visible; see \(runID)-system-tree")

            let waitStarted = Date()
            var targetFound = false
            while Date().timeIntervalSince(waitStarted) < targetTimeout {
                if matchCount(app, containing: target) > targetBaseline {
                    targetFound = true
                    break
                }
                Thread.sleep(forTimeInterval: 1)
            }
            let waitSeconds = Date().timeIntervalSince(waitStarted)
            let serviceSeen = matchCount(app, containing: service) > serviceBaseline
            let emptyStateSeen = app.staticTexts["未找到设备"].exists
            let observedAt = ISO8601DateFormatter().string(from: Date())

            if targetFound {
                let found = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
                found.name = runID + "-target-found"
                found.lifetime = .keepAlways
                add(found)
            }

            let evidence: [String: Any] = [
                "run_id": runID,
                "source": "xctest.physical",
                "mode": "m4",
                "target_name": target,
                "service": service,
                "observation": targetFound ? "target_found" : "target_absent",
                "target_found": targetFound,
                "service_seen_in_picker": serviceSeen,
                "empty_state_seen": emptyStateSeen,
                "picker_verified": pickerShown,
                "device_tap_count": 0,
                "pairing_flow_started": false,
                "observed_at_utc": observedAt,
                "monotonic_ns": DispatchTime.now().uptimeNanoseconds,
                "wait_seconds": waitSeconds,
                "wait_timeout_seconds": targetTimeout,
            ]
            let data = try JSONSerialization.data(withJSONObject: evidence, options: [.sortedKeys])
            let result = XCTAttachment(data: data, uniformTypeIdentifier: "public.json")
            result.name = runID + "-m4-result"
            result.lifetime = .keepAlways
            add(result)

            XCTAssertTrue(app.buttons["关闭"].exists)
            app.buttons["关闭"].tap()
            XCTAssertTrue(app.buttons["discovery.finish"].waitForExistence(timeout: 5))
            app.buttons["discovery.finish"].tap()
            XCTAssertTrue(app.staticTexts["session.state"].waitForExistence(timeout: 5))
            XCTAssertEqual(app.staticTexts["session.state"].label, "cancelled")
            app.terminate()
        }
    }

    /// Observes one system picker session while an external board gate toggles
    /// NAN SDF resume/stop. The test never taps the picker; each transition is
    /// captured with a screenshot and UI tree.
    @MainActor func testGateObservation() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_GATE_COUNT"] ?? "10") ?? 10
        XCTAssertTrue((1...10).contains(count))
        let approval = env["AWARE_LONG_WAIT_APPROVED"] ?? ""
        let approved = !approval.isEmpty
        if !approved { XCTAssertEqual(count, 1) }
        let ttlMode = env["AWARE_GATE_TTL"] == "1"
        if ttlMode { XCTAssertEqual(count, 1) }
        let target = env["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName
        let timeout = min(max(Double(env["AWARE_GATE_TIMEOUT"] ?? "5") ?? 5, 5), 30)
        let runID = batchID + "-gate"

        let app = XCUIApplication()
        app.launchEnvironment["AWARE_RUN_ID"] = runID
        app.launchEnvironment["AWARE_EXPERIMENT_ID"] = batchID
        app.launchEnvironment["AWARE_MODE"] = "m4"
        app.launchEnvironment["AWARE_GATE_MODE"] = "1"
        app.launchEnvironment["AWARE_TARGET_NAME"] = target
        app.launchEnvironment["AWARE_DISCOVERY_TIMEOUT"] = "30"
        if approved { app.launchEnvironment["AWARE_LONG_WAIT_APPROVED"] = approval }
        app.launch()
        defer { app.terminate() }

        XCTAssertTrue(app.descendants(matching: .any)["capability.check"].waitForExistence(timeout: 5))
        app.descendants(matching: .any)["capability.check"].tap()
        XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
        XCTAssertEqual(app.staticTexts["session.state"].label, "ready")

        let baseline = matchCount(app, containing: target)
        app.buttons["discovery.begin"].tap()
        XCTAssertTrue(app.buttons["discovery.system"].waitForExistence(timeout: 5))
        app.buttons["discovery.system"].tap()
        XCTAssertTrue(app.buttons["关闭"].waitForExistence(timeout: 5))

        var cycles: [[String: Any]] = []
        var failure: String?
        for gate in 1...count {
            let onStart = Date()
            let onStartMon = DispatchTime.now().uptimeNanoseconds
            var present = matchCount(app, containing: target) > baseline
            while !present, Date().timeIntervalSince(onStart) < timeout {
                Thread.sleep(forTimeInterval: 0.5)
                present = matchCount(app, containing: target) > baseline
            }
            let onObserved = Date()
            let onObservedMon = DispatchTime.now().uptimeNanoseconds
            let onSeconds = onObserved.timeIntervalSince(onStart)
            addAttachment(app, name: String(format: "%@-gate-%02d-on", runID, gate))
            if !present {
                failure = "gate \(gate) target did not appear"
                addAttachment(app, name: String(format: "%@-gate-%02d-on-failure", runID, gate))
                break
            }

            let offStart = Date()
            let offStartMon = DispatchTime.now().uptimeNanoseconds
            var gone = matchCount(app, containing: target) <= baseline
            while !gone, Date().timeIntervalSince(offStart) < timeout {
                Thread.sleep(forTimeInterval: 0.5)
                gone = matchCount(app, containing: target) <= baseline
            }
            let offObserved = Date()
            let offObservedMon = DispatchTime.now().uptimeNanoseconds
            let offSeconds = offObserved.timeIntervalSince(offStart)
            addAttachment(app, name: String(format: "%@-gate-%02d-off", runID, gate))
            if !gone && !ttlMode {
                failure = "gate \(gate) target did not disappear"
                addAttachment(app, name: String(format: "%@-gate-%02d-off-failure", runID, gate))
                break
            }
            let offDisappeared = gone
            cycles.append([
                "gate": gate,
                "target_found": true,
                "on_wait_started_at_utc": ISO8601DateFormatter().string(from: onStart),
                "on_wait_started_monotonic_ns": onStartMon,
                "on_observed_at_utc": ISO8601DateFormatter().string(from: onObserved),
                "on_observed_monotonic_ns": onObservedMon,
                "on_wait_seconds": onSeconds,
                "on_timeout_seconds": timeout,
                "off_wait_started_at_utc": ISO8601DateFormatter().string(from: offStart),
                "off_wait_started_monotonic_ns": offStartMon,
                "off_disappeared": offDisappeared,
                "off_observed_at_utc": offDisappeared ? ISO8601DateFormatter().string(from: offObserved) : NSNull(),
                "off_observed_monotonic_ns": offDisappeared ? offObservedMon : NSNull(),
                "off_wait_seconds": offSeconds,
                "off_timeout_seconds": timeout,
            ])
        }

        let finalAbsent = matchCount(app, containing: target) <= baseline
        let evidence: [String: Any] = [
            "run_id": runID,
            "source": "xctest.physical",
            "mode": "m4-gate",
            "target_name": target,
            "service": service,
            "gate_count": count,
            "gate_timeout_seconds": timeout,
            "ttl_mode": ttlMode,
            "gate_cycles": cycles,
            "successful_cycles": cycles.count,
            "final_target_absent": finalAbsent,
            "picker_verified": true,
            "device_tap_count": 0,
            "pairing_flow_started": false,
            "failure": failure ?? NSNull(),
            "monotonic_ns": DispatchTime.now().uptimeNanoseconds,
            "observed_at_utc": ISO8601DateFormatter().string(from: Date()),
        ]
        let data = try JSONSerialization.data(withJSONObject: evidence, options: [.sortedKeys])
        let result = XCTAttachment(data: data, uniformTypeIdentifier: "public.json")
        result.name = runID + "-gate-result"
        result.lifetime = .keepAlways
        add(result)

        if let failure = failure {
            XCTFail(failure)
            return
        }
        XCTAssertEqual(cycles.count, count)
        if !ttlMode {
            XCTAssertTrue(finalAbsent, "last gate off did not clear target")
        }
        app.buttons["关闭"].tap()
        XCTAssertTrue(app.buttons["discovery.finish"].waitForExistence(timeout: 5))
        app.buttons["discovery.finish"].tap()
    }

    private func addAttachment(_ app: XCUIApplication, name: String) {
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = name
        shot.lifetime = .keepAlways
        add(shot)
        let tree = XCTAttachment(string: app.debugDescription)
        tree.name = name + "-tree"
        tree.lifetime = .keepAlways
        add(tree)
    }

    private func matchCount(_ app: XCUIApplication, containing text: String) -> Int {
        let predicate = NSPredicate(format: "label CONTAINS %@", text)
        return app.staticTexts.matching(predicate).count + app.cells.matching(predicate).count
    }
}
