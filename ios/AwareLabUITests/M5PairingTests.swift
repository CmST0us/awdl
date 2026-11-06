import XCTest

/// M5 AccessorySetupKit discovery, controlled PIN pairing and paired-device evidence.
final class M5PairingTests: XCTestCase {
    private struct TargetRow {
        let element: XCUIElement
        let query: String
    }

    private struct PINField {
        let element: XCUIElement
        let scope: String
    }

    private let service = TestConfiguration.service
    private let displayItemName = TestConfiguration.displayItemName
    private let vendorName = TestConfiguration.vendorName
    private let modelName = TestConfiguration.modelName

    @MainActor func testAccessoryPickerObservation() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_REPEAT_COUNT"] ?? "1") ?? 1
        XCTAssertTrue((1...10).contains(count))
        let target = env["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName
        let timeout = min(max(Double(env["AWARE_TARGET_TIMEOUT"] ?? "5") ?? 5, 5), 5)
        let candidates = targetCandidates(target)

        for index in 1...count {
            let runID = String(format: "%@-%02d", batchID, index)
            let app = application(runID: runID, batchID: batchID, target: target)
            app.launch()
            defer { app.terminate() }

            XCTAssertTrue(app.buttons["capability.check"].waitForExistence(timeout: 5))
            XCTAssertTrue(app.staticTexts["run.id"].label.contains(runID))
            XCTAssertTrue(app.staticTexts["mode.value"].label.contains("m5"))
            XCTAssertTrue(app.staticTexts["m5.target"].label.contains(target))
            app.buttons["capability.check"].tap()
            XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
            XCTAssertEqual(app.staticTexts["session.state"].label, "ready")
            XCTAssertTrue(app.staticTexts["pairing.state"].waitForExistence(timeout: 5))
            XCTAssertEqual(app.staticTexts["pairing.state"].label, "idle")

            var baseline: [String: Int] = [:]
            for candidate in candidates {
                baseline[candidate] = matchCount(app, containing: candidate)
            }

            app.buttons["pair.begin"].tap()
            let waitStart = Date()
            let waitStartNS = DispatchTime.now().uptimeNanoseconds
            let deadline = waitStart.addingTimeInterval(timeout)
            var history: [String] = []
            var matched: Set<String> = []
            var pickerPresented = false
            var pickerDismissed = false
            var observationCaptured = false
            while Date() < deadline {
                let state = label(app, "pairing.state")
                if state != history.last { history.append(state) }
                let active = ["pickerPresented", "pickerDismissed", "authorized"].contains(state)
                if active {
                    pickerPresented = true
                    pickerDismissed = pickerDismissed || state == "pickerDismissed"
                    for candidate in candidates
                    where matchCount(app, containing: candidate) > (baseline[candidate] ?? 0) {
                        matched.insert(candidate)
                    }
                    if !observationCaptured {
                        capture(app, name: runID + "-ask-observation")
                        observationCaptured = true
                    }
                    if state != "pickerPresented" { break }
                }
                if state == "failed" || state == "invalidated" { break }
                Thread.sleep(forTimeInterval: 0.5)
            }
            let waitSeconds = Date().timeIntervalSince(waitStart)
            if !observationCaptured { capture(app, name: runID + "-ask-observation") }
            capture(app, name: runID + "-ask-final")

            let finalState = label(app, "pairing.state")
            let appError = label(app, "pairing.error")
            let accessoryIDs = parseIDs(label(app, "pairing.accessoryIDs"))
            let pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
            let firstPairedID: Any = (accessoryIDs.first ?? pairedIDs.first).map { $0 as Any } ?? NSNull()
            let authorizedCount = Int(label(app, "pairing.authorized")) ?? 0
            var failure: String?
            if finalState.isEmpty {
                failure = "ASK state label was not readable after the picker request"
            } else if finalState == "failed" || finalState == "invalidated" {
                failure = "ASK state \(finalState)" + (appError.isEmpty ? "" : ": \(appError)")
            } else if !pickerPresented {
                failure = "ASK picker did not present within \(Int(timeout)) s (state=\(finalState))"
            }

            let evidence: [String: Any] = [
                "run_id": runID,
                "source": "xctest.physical",
                "mode": "m5",
                "test": "accessory_picker",
                "target_name": target,
                "service": service,
                "display_item_name": displayItemName,
                "vendor_name": vendorName,
                "model_name": modelName,
                "picker_requested": true,
                "picker_presented": pickerPresented,
                "picker_dismissed": pickerDismissed,
                "picker_state_observed": finalState,
                "state_history": history,
                "accessory_count": Int(label(app, "pairing.accessories")) ?? 0,
                "authorized": authorizedCount > 0,
                "authorized_count": authorizedCount,
                "accessory_paired_device_ids": accessoryIDs,
                "paired_device_ids": pairedIDs,
                "wifi_aware_paired_device_id": firstPairedID,
                "snapshot_state": label(app, "pairing.snapshot"),
                "target_seen_in_picker": !matched.isEmpty,
                "target_candidates_seen": matched.sorted(),
                "target_candidates_expected": candidates,
                "device_tap_count": 0,
                "pin_entry_attempted": false,
                "pin_handling": "system_picker",
                "wait_started_monotonic_ns": waitStartNS,
                "wait_seconds": waitSeconds,
                "wait_timeout_seconds": timeout,
                "observed_at_utc": ISO8601DateFormatter().string(from: Date()),
                "monotonic_ns": DispatchTime.now().uptimeNanoseconds,
                "failure": failure ?? NSNull(),
            ]
            addResult(evidence, runID: runID)
            if let failure { XCTFail(failure) }
            app.terminate()
        }
    }

    /// Stage C support: read the App's existing AccessorySetupKit and
    /// WiFiAware records without presenting the picker or entering a PIN.
    @MainActor func testPairedDeviceSnapshot() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_REPEAT_COUNT"] ?? "1") ?? 1
        XCTAssertTrue((1...10).contains(count))
        let target = env["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName
        let timeout = min(max(Double(env["AWARE_TARGET_TIMEOUT"] ?? "5") ?? 5, 5), 5)

        for index in 1...count {
            let runID = String(format: "%@-%02d", batchID, index)
            let app = application(runID: runID, batchID: batchID, target: target)
            app.launch()
            defer { app.terminate() }

            XCTAssertTrue(app.buttons["capability.check"].waitForExistence(timeout: 5))
            app.buttons["capability.check"].tap()
            XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
            XCTAssertEqual(app.staticTexts["session.state"].label, "ready")
            XCTAssertTrue(app.buttons["pair.refresh"].waitForExistence(timeout: 5))
            app.buttons["pair.refresh"].tap()

            let waitStart = Date()
            let waitStartNS = DispatchTime.now().uptimeNanoseconds
            let deadline = waitStart.addingTimeInterval(timeout)
            var snapshot = ""
            var finalState = ""
            while Date() < deadline {
                snapshot = label(app, "pairing.snapshot")
                finalState = label(app, "pairing.state")
                if snapshot == "complete" || snapshot == "failed" { break }
                Thread.sleep(forTimeInterval: 0.5)
            }
            let waitSeconds = Date().timeIntervalSince(waitStart)
            capture(app, name: runID + "-ask-observation")

            let appError = label(app, "pairing.error")
            let accessoryIDs = parseIDs(label(app, "pairing.accessoryIDs"))
            let pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
            let firstPairedID: Any = (accessoryIDs.first ?? pairedIDs.first).map { $0 as Any } ?? NSNull()
            let authorizedCount = Int(label(app, "pairing.authorized")) ?? 0
            var failure: String?
            if snapshot != "complete" {
                failure = "paired-device snapshot \(snapshot.isEmpty ? "unreadable" : snapshot)"
                    + (appError.isEmpty ? "" : ": \(appError)")
            }

            let evidence: [String: Any] = [
                "run_id": runID,
                "source": "xctest.physical",
                "mode": "m5",
                "test": "paired_device_snapshot",
                "target_name": target,
                "service": service,
                "display_item_name": displayItemName,
                "vendor_name": vendorName,
                "model_name": modelName,
                "picker_requested": false,
                "picker_presented": false,
                "picker_dismissed": false,
                "picker_state_observed": finalState,
                "state_history": finalState.isEmpty ? [] : [finalState],
                "accessory_count": Int(label(app, "pairing.accessories")) ?? 0,
                "authorized": authorizedCount > 0,
                "authorized_count": authorizedCount,
                "accessory_paired_device_ids": accessoryIDs,
                "paired_device_ids": pairedIDs,
                "wifi_aware_paired_device_id": firstPairedID,
                "snapshot_state": snapshot,
                "target_seen_in_picker": false,
                "target_candidates_seen": [],
                "target_candidates_expected": targetCandidates(target),
                "device_tap_count": 0,
                "pin_entry_attempted": false,
                "pin_handling": "system_picker",
                "wait_started_monotonic_ns": waitStartNS,
                "wait_seconds": waitSeconds,
                "wait_timeout_seconds": timeout,
                "observed_at_utc": ISO8601DateFormatter().string(from: Date()),
                "monotonic_ns": DispatchTime.now().uptimeNanoseconds,
                "failure": failure ?? NSNull(),
            ]
            addResult(evidence, runID: runID)
            if let failure { XCTFail(failure) }
            app.terminate()
        }
    }

    /// Stage A: tap the known board row in the ASK picker, enter the PIN that
    /// run_tests.py injects as AWARE_M5_PIN, submit it and record the system
    /// outcome. The PIN is kept in test memory only and is redacted from every
    /// attachment. A missing board, PIN UI or authorization is evidence with a
    /// distinct failure category, never a fabricated success.
    @MainActor func testStageAPairing() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let count = Int(env["AWARE_REPEAT_COUNT"] ?? "1") ?? 1
        XCTAssertTrue((1...10).contains(count))
        let target = env["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName
        let targetTimeout = min(max(Double(env["AWARE_TARGET_TIMEOUT"] ?? "5") ?? 5, 5), 5)
        let pinTimeout: TimeInterval = 5
        let authorizeTimeout: TimeInterval = 5
        let resetExisting = env["AWARE_M5_RESET_EXISTING"] == "1"
        guard let pin = env["AWARE_M5_PIN"],
              pin.range(of: #"^[0-9]{6}$"#, options: .regularExpression) != nil else {
            XCTFail("Stage A requires a 6-digit AWARE_M5_PIN environment value")
            return
        }
        let candidates = stageATargetCandidates(target)

        for index in 1...count {
            let runID = String(format: "%@-%02d", batchID, index)
            let app = application(runID: runID, batchID: batchID, target: target)
            app.launch()
            defer { app.terminate() }

            XCTAssertTrue(app.buttons["capability.check"].waitForExistence(timeout: 5))
            XCTAssertTrue(app.staticTexts["run.id"].label.contains(runID))
            XCTAssertTrue(app.staticTexts["mode.value"].label.contains("m5"))
            XCTAssertTrue(app.staticTexts["m5.target"].label.contains(target))
            app.buttons["capability.check"].tap()
            XCTAssertEqual(app.staticTexts["capability.value"].label, "supported")
            XCTAssertEqual(app.staticTexts["session.state"].label, "ready")
            XCTAssertTrue(app.staticTexts["pairing.state"].waitForExistence(timeout: 5))
            if resetExisting {
                let remove = app.buttons["pair.remove"]
                XCTAssertTrue(remove.waitForExistence(timeout: 5))
                remove.tap()
                confirmUnpairIfPresent()
                let cleared = XCTNSPredicateExpectation(
                    predicate: NSPredicate { _, _ in
                        MainActor.assumeIsolated {
                            self.label(app, "pairing.accessories") == "0" &&
                            self.label(app, "pairing.pairedIDs") == "none"
                        }
                    }, object: nil)
                XCTAssertEqual(XCTWaiter.wait(for: [cleared], timeout: 5), .completed)
            }

            let evidence = runStageAIteration(app: app, runID: runID, target: target,
                                              candidates: candidates, pin: pin,
                                              targetTimeout: targetTimeout,
                                              pinTimeout: pinTimeout,
                                              authorizeTimeout: authorizeTimeout)
            addResult(evidence, runID: runID)
            if let failure = evidence["failure"] as? String, !failure.isEmpty {
                XCTFail(failure)
            }
            app.terminate()
        }
    }

    @MainActor private func runStageAIteration(app: XCUIApplication, runID: String,
                                               target: String, candidates: [String],
                                               pin: String, targetTimeout: TimeInterval,
                                               pinTimeout: TimeInterval,
                                               authorizeTimeout: TimeInterval) -> [String: Any] {
        let startDate = Date()
        let runTimeout = Double(ProcessInfo.processInfo.environment["AWARE_RUN_TIMEOUT"] ?? "30") ?? 30
        var history: [String] = []
        var observationCaptured = false
        var pinField: XCUIElement?
        var evidence: [String: Any] = [
            "run_id": runID,
            "source": "xctest.physical",
            "mode": "m5",
            "test": "stage_a_pairing",
            "target_name": target,
            "service": service,
            "display_item_name": displayItemName,
            "vendor_name": vendorName,
            "model_name": modelName,
            "pin_injected": true,
            "pin_source": "AWARE_M5_PIN",
            "pin_length": pin.count,
            "pin_handling": "system_picker",
            "picker_requested": true,
            "picker_presented": false,
            "picker_search_tapped": false,
            "picker_search_button": NSNull(),
            "picker_dismissed": false,
            "picker_state_observed": "",
            "state_history": [String](),
            "target_row_found": false,
            "target_row_query": NSNull(),
            "target_seen_in_picker": false,
            "target_candidates_seen": [String](),
            "target_candidates_expected": candidates,
            "device_tap_count": 0,
            "pairing_flow_started": false,
            "pin_ui_found": false,
            "pin_ui_scope": NSNull(),
            "pin_entry_attempted": false,
            "pin_submit_method": NSNull(),
            "pin_error_signal": false,
            "pin_error_text": NSNull(),
            "system_alert_labels": [String](),
            "post_submit_pin_field_present": false,
            "screenshot_withheld_due_to_unmasked_pin": false,
            "accessory_count": 0,
            "authorized": false,
            "authorized_count": 0,
            "accessory_paired_device_ids": [UInt64](),
            "paired_device_ids": [UInt64](),
            "wifi_aware_paired_device_id": NSNull(),
            "snapshot_state": "pending",
            "failure_category": NSNull(),
            "failure": NSNull(),
            "wait_seconds": 0.0,
            "wait_timeout_seconds": runTimeout,
            "target_timeout_seconds": targetTimeout,
            "pin_timeout_seconds": pinTimeout,
            "authorize_timeout_seconds": authorizeTimeout,
            "started_utc": "", "started_monotonic_ns": 0,
            "picker_requested_utc": "", "picker_requested_monotonic_ns": 0,
            "picker_presented_utc": "", "picker_presented_monotonic_ns": 0,
            "picker_search_tapped_utc": "", "picker_search_tapped_monotonic_ns": 0,
            "target_found_utc": "", "target_found_monotonic_ns": 0,
            "target_tap_utc": "", "target_tap_monotonic_ns": 0,
            "pin_ui_found_utc": "", "pin_ui_found_monotonic_ns": 0,
            "pin_typed_utc": "", "pin_typed_monotonic_ns": 0,
            "pin_submitted_utc": "", "pin_submitted_monotonic_ns": 0,
            "result_utc": "", "result_monotonic_ns": 0,
            "observed_at_utc": "",
            "monotonic_ns": 0,
        ]

        func observe() -> String {
            let state = label(app, "pairing.state")
            if state != history.last { history.append(state) }
            evidence["state_history"] = history
            evidence["picker_state_observed"] = state
            return state
        }
        func stamp(_ key: String) {
            evidence[key + "_utc"] = ISO8601DateFormatter().string(from: Date())
            evidence[key + "_monotonic_ns"] = DispatchTime.now().uptimeNanoseconds
        }
        func captureObservation() {
            if !observationCaptured {
                capture(app, name: runID + "-ask-observation", pin: pin)
                observationCaptured = true
            }
        }
        func captureFinal() {
            if let field = pinField, field.exists, field.elementType != .secureTextField {
                evidence["screenshot_withheld_due_to_unmasked_pin"] = true
                attachTree(app, name: runID + "-ask-final", pin: pin)
            } else {
                capture(app, name: runID + "-ask-final", pin: pin)
            }
        }
        func finish(_ category: String?, _ detail: String) -> [String: Any] {
            let finalState = observe()
            let authorizedCount = Int(label(app, "pairing.authorized")) ?? 0
            let accessoryIDs = parseIDs(label(app, "pairing.accessoryIDs"))
            let pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
            let firstID: Any = (accessoryIDs.first ?? pairedIDs.first).map { $0 as Any } ?? NSNull()
            evidence["picker_state_observed"] = finalState
            evidence["state_history"] = history
            evidence["picker_dismissed"] = history.contains("pickerDismissed")
            evidence["accessory_count"] = Int(label(app, "pairing.accessories")) ?? 0
            evidence["authorized"] = authorizedCount > 0
            evidence["authorized_count"] = authorizedCount
            evidence["accessory_paired_device_ids"] = accessoryIDs
            evidence["paired_device_ids"] = pairedIDs
            evidence["wifi_aware_paired_device_id"] = firstID
            evidence["snapshot_state"] = label(app, "pairing.snapshot")
            evidence["wait_seconds"] = Date().timeIntervalSince(startDate)
            evidence["observed_at_utc"] = ISO8601DateFormatter().string(from: Date())
            evidence["monotonic_ns"] = DispatchTime.now().uptimeNanoseconds
            stamp("result")
            if let category {
                evidence["failure_category"] = category
                evidence["failure"] = redact(detail, pin: pin)
            } else {
                evidence["failure_category"] = NSNull()
                evidence["failure"] = NSNull()
            }
            return evidence
        }
        func fail(_ category: String, _ detail: String) -> [String: Any] {
            captureObservation()
            captureFinal()
            return finish(category, detail)
        }

        stamp("started")
        _ = observe()
        app.buttons["pair.begin"].tap()
        stamp("picker_requested")

        var pickerPresented = false
        var state = ""
        let pickerDeadline = Date().addingTimeInterval(targetTimeout)
        while Date() < pickerDeadline {
            state = observe()
            if state == "pickerPresented" { pickerPresented = true; break }
            if state == "failed" || state == "invalidated" { break }
            Thread.sleep(forTimeInterval: 0.5)
        }
        evidence["picker_presented"] = pickerPresented
        if pickerPresented {
            stamp("picker_presented")
            var searchButton: (element: XCUIElement, label: String)?
            let searchDeadline = Date().addingTimeInterval(5)
            while Date() < searchDeadline {
                searchButton = findPickerSearchButton(app)
                if searchButton != nil { break }
                Thread.sleep(forTimeInterval: 0.5)
            }
            if let searchButton {
                searchButton.element.tap()
                evidence["picker_search_tapped"] = true
                evidence["picker_search_button"] = searchButton.label
                stamp("picker_search_tapped")
            }
        } else {
            let appError = redact(label(app, "pairing.error"), pin: pin)
            let category = (state == "failed" || state == "invalidated") ? "ask_failed" : "picker_not_presented"
            return fail(category, "\(category): state=\(state)" + (appError.isEmpty ? "" : "; error=\(appError)"))
        }

        var targetRow: TargetRow?
        var targetSeen = false
        let targetDeadline = Date().addingTimeInterval(targetTimeout)
        while Date() < targetDeadline {
            let found = findTargetRow(app, candidates: candidates)
            targetSeen = targetSeen || found.seen
            if let row = found.row { targetRow = row; break }
            state = observe()
            if state == "failed" || state == "invalidated" { break }
            Thread.sleep(forTimeInterval: 0.5)
        }
        evidence["target_seen_in_picker"] = targetSeen
        guard let row = targetRow else {
            let category = targetSeen ? "target_not_hittable" : "target_not_listed"
            let stateNow = observe()
            return fail(category, "\(category): state=\(stateNow); targetSeen=\(targetSeen); pickerPresented=\(pickerPresented)")
        }
        evidence["target_row_found"] = true
        evidence["target_row_query"] = row.query
        evidence["target_candidates_seen"] = candidates.filter { row.query.contains($0) }
        stamp("target_found")
        captureObservation()
        row.element.tap()
        evidence["device_tap_count"] = 1
        evidence["pairing_flow_started"] = true
        stamp("target_tap")

        let pinDeadline = Date().addingTimeInterval(pinTimeout)
        while Date() < pinDeadline {
            if let found = findPINField(app) {
                pinField = found.element
                evidence["pin_ui_found"] = true
                evidence["pin_ui_scope"] = found.scope
                stamp("pin_ui_found")
                break
            }
            state = observe()
            if state == "failed" || state == "invalidated" { break }
            Thread.sleep(forTimeInterval: 0.5)
        }
        guard let field = pinField, evidence["pin_ui_found"] as? Bool == true else {
            let stateNow = observe()
            return fail("pin_ui_not_found", "pin_ui_not_found: state=\(stateNow); target=\(target)")
        }
        capture(app, name: runID + "-ask-pin", pin: pin)
        field.tap()
        // Type digit by digit so XCTest's activity log never records the whole PIN in one entry.
        for character in pin {
            field.typeText(String(character))
        }
        evidence["pin_entry_attempted"] = true
        stamp("pin_typed")
        let submitMethod = submitPIN(field, app: app)
        evidence["pin_submit_method"] = submitMethod
        stamp("pin_submitted")

        var alertLabels: [String] = []
        var pinErrorSignal = false
        var pinErrorText: String?
        var pinFieldGone = false
        var poll = 0
        let resultDeadline = Date().addingTimeInterval(authorizeTimeout)
        while Date() < resultDeadline {
            state = observe()
            if (Int(label(app, "pairing.authorized")) ?? 0) > 0 { break }
            if poll % 2 == 0 {
                alertLabels = collectAlertLabels(app)
                if !alertLabels.isEmpty {
                    evidence["system_alert_labels"] =
                        Array(alertLabels.map { redact($0, pin: pin) }.prefix(3))
                }
                if hasPINErrorWord(alertLabels) {
                    pinErrorSignal = true
                    pinErrorText = alertLabels.first
                    break
                }
                if let errorText = findPINErrorText(app) {
                    pinErrorSignal = true
                    pinErrorText = errorText
                    break
                }
            }
            pinFieldGone = pinFieldGone || !field.exists
            if state == "failed" || state == "invalidated" { break }
            poll += 1
            Thread.sleep(forTimeInterval: 0.5)
        }
        pinFieldGone = pinFieldGone || !field.exists
        evidence["pin_error_signal"] = pinErrorSignal
        if let pinErrorText {
            evidence["pin_error_text"] = redact(String(pinErrorText.prefix(200)), pin: pin)
        }
        evidence["post_submit_pin_field_present"] = field.exists

        let authorizedCount = Int(label(app, "pairing.authorized")) ?? 0
        if authorizedCount > 0 {
            var snapshotComplete = label(app, "pairing.snapshot") == "complete"
            var pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
            let snapshotDeadline = Date().addingTimeInterval(5)
            while Date() < snapshotDeadline {
                snapshotComplete = label(app, "pairing.snapshot") == "complete"
                pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
                if snapshotComplete && !pairedIDs.isEmpty { break }
                Thread.sleep(forTimeInterval: 0.5)
            }
            let accessoryIDs = parseIDs(label(app, "pairing.accessoryIDs"))
            if Set(accessoryIDs).intersection(pairedIDs).subtracting([0]).isEmpty || !snapshotComplete {
                captureFinal()
                return finish("authorized_without_paired_device_id",
                              "authorized_without_paired_device_id: accessoryIDs=\(accessoryIDs.count); pairedIDs=\(pairedIDs.count); snapshot=\(label(app, "pairing.snapshot"))")
            }
            captureFinal()
            return finish(nil, "")
        }

        let stateNow = observe()
        let category: String
        if pinErrorSignal {
            category = "pin_rejected"
        } else if stateNow == "failed" || stateNow == "invalidated" {
            category = "ask_failed"
        } else {
            category = "authorized_timeout"
        }
        let appError = redact(label(app, "pairing.error"), pin: pin)
        let detail = "\(category): state=\(stateNow); pinFieldGone=\(pinFieldGone); "
            + "alerts=\(alertLabels.count); pinError=\(pinErrorSignal)"
            + (appError.isEmpty ? "" : "; appError=\(appError)")
        return fail(category, detail)
    }

    @MainActor func testCommonClient() throws {
        continueAfterFailure = false
        let client = XCUIApplication(bundleIdentifier: ProcessInfo.processInfo.environment["AWARE_CLIENT_BUNDLE_ID"] ?? TestConfiguration.clientBundle)
        client.launch()
        defer {
            let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
            let description = client.debugDescription + "\n" + springboard.debugDescription
            let tree = XCTAttachment(string: redact(description, pin: ProcessInfo.processInfo.environment["AWARE_M5_PIN"] ?? ""))
            tree.name = "common-client-ui"
            tree.lifetime = .keepAlways
            add(tree)
            let screen = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            screen.lifetime = .keepAlways
            add(screen)
        }
        XCTAssertTrue(client.buttons["配对新设备"].waitForExistence(timeout: 5))
        let action = ProcessInfo.processInfo.environment["AWARE_CLIENT_ACTION"] ?? "observe"
        if action == "pair" {
            let pin = try XCTUnwrap(ProcessInfo.processInfo.environment["AWARE_M5_PIN"])
            client.buttons["配对新设备"].tap()
            var search: (element: XCUIElement, label: String)?
            let finding = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated { search = self.findPickerSearchButton(client); return search != nil }
            }, object: nil)
            XCTAssertEqual(XCTWaiter.wait(for: [finding], timeout: 5), .completed)
            try XCTUnwrap(search).element.tap()
            let system = XCUIApplication(bundleIdentifier: "com.apple.springboard")
            let tray = system.otherElements["PRXCardBottomTray"]
            let setup = tray.buttons.matching(NSPredicate(format: "label IN %@",
                ["设置", "設定", "Set Up", "与App共享", "Share with App"])).firstMatch
            XCTAssertTrue(setup.waitForExistence(timeout: 5))
            let sharedIdentity = ["与App共享", "Share with App"].contains(setup.label)
            setup.tap()
            if !sharedIdentity {
                var input: PINField?
                let entering = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                    MainActor.assumeIsolated { input = self.findPINField(client); return input != nil }
                }, object: nil)
                XCTAssertEqual(XCTWaiter.wait(for: [entering], timeout: 5), .completed)
                let field = try XCTUnwrap(input).element
                field.tap()
                field.typeText(String(pin.prefix(3)))
                field.typeText(String(pin.suffix(3)))
                _ = submitPIN(field, app: client)
                let done = tray.buttons.matching(NSPredicate(format: "label IN %@",
                    ["完成", "Done"])).firstMatch
                XCTAssertTrue(done.waitForExistence(timeout: 5))
                done.tap()
            }
            let authorized = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated { client.buttons["配对新设备"].isHittable }
            }, object: nil)
            XCTAssertEqual(XCTWaiter.wait(for: [authorized], timeout: 5), .completed)
        }
        if let target = ProcessInfo.processInfo.environment["AWARE_CLIENT_DEVICE_ID"] {
            client.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", "已配对设备")).firstMatch.tap()
            let device = client.buttons.matching(NSPredicate(format: "label ENDSWITH %@", "ID " + target)).firstMatch
            XCTAssertTrue(device.waitForExistence(timeout: 5))
            device.tap()
        }
        if action == "connect" { client.buttons["连接"].tap() }
        if ["download", "upload", "cancel", "reconnect", "speed"].contains(action) {
            client.buttons["连接"].tap()
            let discovered = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated {
                    client.staticTexts["正在读取文件列表"].exists ||
                    client.buttons.containing(.staticText, identifier: "alignment.txt").firstMatch.exists
                }
            }, object: nil)
            XCTAssertEqual(XCTWaiter.wait(for: [discovered], timeout: 5), .completed)
            let file = client.buttons.containing(.staticText, identifier: "alignment.txt").firstMatch
            XCTAssertTrue(file.waitForExistence(timeout: 5))
            if action == "speed" {
                let payload = client.buttons.containing(.staticText, identifier: "speed-1g.bin").firstMatch
                XCTAssertTrue(payload.exists)
                payload.tap()
                // 旧路径 1GiB 为 24–31 秒；公共路径首次基线保留约三倍失败预算。
                XCTAssertTrue(client.buttons["打开下载文件"].waitForExistence(timeout: 90))
                let rate = client.staticTexts.matching(NSPredicate(format: "label BEGINSWITH %@", "下载 ")).firstMatch
                XCTAssertTrue(rate.exists)
                print("COMMON_SPEED_RESULT: " + rate.label)
            } else if action == "download" {
                file.tap()
                XCTAssertTrue(client.buttons["打开下载文件"].waitForExistence(timeout: 5))
            } else if action == "cancel" {
                client.buttons.containing(.staticText, identifier: "cancel.bin").firstMatch.tap()
                let cancel = client.buttons["取消"]
                XCTAssertTrue(cancel.waitForExistence(timeout: 5))
                cancel.tap()
                XCTAssertTrue(client.staticTexts["已取消"].waitForExistence(timeout: 5))
            } else if action == "reconnect" {
                client.buttons["断开"].tap()
                XCTAssertTrue(client.buttons["连接"].waitForExistence(timeout: 5))
                client.buttons["连接"].tap()
                XCTAssertTrue(file.waitForExistence(timeout: 5))
            } else {
                client.buttons["上传文件"].tap()
                let local = client.cells.matching(NSPredicate(format: "label IN %@",
                    ["我的 iPad", "我的iPad", "On My iPad"])).firstMatch
                XCTAssertTrue(local.waitForExistence(timeout: 5))
                local.tap()
                let folder = client.cells.matching(NSPredicate(format: "label CONTAINS %@", ProcessInfo.processInfo.environment["AWARE_CLIENT_FOLDER"] ?? TestConfiguration.clientDisplayName)).firstMatch
                XCTAssertTrue(folder.waitForExistence(timeout: 5))
                folder.tap()
                let source = client.cells.matching(NSPredicate(format: "label CONTAINS %@", "upload-probe")).firstMatch
                XCTAssertTrue(source.waitForExistence(timeout: 5))
                source.tap()
                let open = client.buttons.matching(NSPredicate(format: "label IN %@", ["打开", "Open"])).firstMatch
                if open.exists { open.tap() }
                XCTAssertTrue(client.staticTexts["上传完成"].waitForExistence(timeout: 5))
            }
        }

    }

    @MainActor func testControlledPairing() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let pin = try XCTUnwrap(env["AWARE_M5_PIN"])
        XCTAssertNotNil(pin.range(of: #"^[0-9]{6}$"#, options: .regularExpression))
        let runID = batchID + "-pairing"
        let app = application(runID: runID, batchID: batchID, target: TestConfiguration.targetName)
        func marker(_ event: String, _ fields: [String: Any] = [:]) {
            var row: [String: Any] = ["run_id": runID, "experiment_id": batchID,
                "source": "xctest.physical", "event": event, "service": service,
                "source_epoch": Date().timeIntervalSince1970,
                "monotonic_ns": DispatchTime.now().uptimeNanoseconds]
            row.merge(fields) { _, new in new }
            if let data = try? JSONSerialization.data(withJSONObject: row, options: [.sortedKeys]) {
                FileHandle.standardOutput.write(Data("AWARE_OBSERVER ".utf8) + data + Data([10]))
            }
        }
        func wait(_ condition: @escaping @MainActor () -> Bool) -> Bool {
            let expectation = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated {
                    condition() || ["failed", "invalidated"].contains(self.label(app, "pairing.state"))
                }
            }, object: nil)
            return XCTWaiter.wait(for: [expectation], timeout: 5) == .completed
        }
        func failed(_ stage: String) {
            marker("pairing_observed", ["authorized": false, "failure": stage,
                "state": label(app, "pairing.state"), "paired_device_ids": [UInt64](),
                "error": redact(label(app, "pairing.error"), pin: pin)])
        }
        app.launch()
        defer { app.terminate() }
        guard app.buttons["pair.begin"].waitForExistence(timeout: 5) else {
            failed("app_not_ready"); return
        }
        if env["AWARE_M5_RESET_EXISTING"] == "1" {
            let refresh = app.buttons["pair.refresh"]
            guard refresh.waitForExistence(timeout: 5) else {
                failed("refresh_button_unavailable"); return
            }
            refresh.tap()
            let remove = app.buttons["pair.remove"]
            let populated = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated {
                    (Int(self.label(app, "pairing.accessories")) ?? 0) > 0 &&
                    remove.isEnabled
                }
            }, object: nil)
            guard remove.waitForExistence(timeout: 5),
                  XCTWaiter.wait(for: [populated], timeout: 5) == .completed,
                  remove.isHittable else {
                failed("remove_button_unavailable"); return
            }
            marker("pairing_reset_requested")
            remove.tap()
            confirmUnpairIfPresent()
            let cleared = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
                MainActor.assumeIsolated {
                    self.label(app, "pairing.accessories") == "0" &&
                    self.label(app, "pairing.pairedIDs") == "none"
                }
            }, object: nil)
            guard XCTWaiter.wait(for: [cleared], timeout: 5) == .completed else {
                failed("pairing_reset_not_completed"); return
            }
            marker("pairing_reset_completed")
        }
        app.buttons["pair.begin"].tap()
        var pickerSearch: (element: XCUIElement, label: String)?
        guard wait({ pickerSearch = self.findPickerSearchButton(app); return pickerSearch != nil }),
              let search = pickerSearch else { failed("search_not_found"); return }
        let existingIDs = Set(parseIDs(label(app, "pairing.pairedIDs")))
        marker("pairing_baseline", ["paired_device_ids": Array(existingIDs)])
        let searchStart = Date().timeIntervalSince1970
        search.element.tap()
        marker("observer_search_triggered", ["search_started_epoch": searchStart])
        let system = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let target = system.staticTexts.matching(
            NSPredicate(format: "label CONTAINS[c] %@", TestConfiguration.targetName)).firstMatch
        guard target.waitForExistence(timeout: 5) else { failed("target_not_found"); return }
        marker("pairing_target_found", ["query": "springboard:staticText:\(TestConfiguration.targetName)"])
        let setup = system.otherElements["PRXCardBottomTray"].buttons.matching(
            NSPredicate(format: "label IN %@", ["设置", "設定", "Set Up"])).firstMatch
        guard setup.exists && setup.isHittable else {
            marker("pairing_ui_tree", ["tree": redact(system.debugDescription, pin: pin)])
            failed("setup_button_not_found"); return
        }
        marker("pairing_setup_requested")
        setup.tap()
        marker("pairing_setup_tapped")
        var input: PINField?
        guard wait({ input = self.findPINField(app); return input != nil }), let input else {
            marker("pairing_ui_tree", ["tree": redact(system.debugDescription, pin: pin)])
            failed("pin_ui_not_found"); return
        }
        marker("pairing_pin_ui", ["scope": input.scope])
        input.element.tap()
        // Keep the complete PIN out of individual XCTest activity entries.
        input.element.typeText(String(pin.prefix(3)))
        input.element.typeText(String(pin.suffix(3)))
        marker("pairing_pin_typed")
        let method = submitPIN(input.element, app: app)
        marker("pairing_pin_submitted", ["method": method])
        _ = wait {
            !Set(self.parseIDs(self.label(app, "pairing.pairedIDs")))
                .subtracting(existingIDs).subtracting([0]).isEmpty ||
                ["failed", "invalidated"].contains(self.label(app, "pairing.state"))
        }
        let accessoryIDs = parseIDs(label(app, "pairing.accessoryIDs"))
        let pairedIDs = parseIDs(label(app, "pairing.pairedIDs"))
        let common = Set(accessoryIDs).intersection(pairedIDs)
            .subtracting(existingIDs).subtracting([0])
        marker("pairing_observed", ["authorized": !common.isEmpty,
            "state": label(app, "pairing.state"), "paired_device_ids": Array(common),
            "error": redact(label(app, "pairing.error"), pin: pin)])
    }

    /// The coordinator enables the publisher only after this marker and a fresh
    /// system subscribe-started event. UI rows are supplementary observations.
    @MainActor func testPublisherDiscoveryObservation() throws {
        continueAfterFailure = false
        let env = ProcessInfo.processInfo.environment
        let batchID = try XCTUnwrap(env["AWARE_BATCH_ID"])
        let runID = batchID + "-discovery"
        let target = TestConfiguration.targetName
        let app = application(runID: runID, batchID: batchID, target: target)
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["pair.begin"].waitForExistence(timeout: 5))
        XCTAssertTrue(label(app, "run.id").contains(runID))
        app.buttons["pair.begin"].tap()
        let presented = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == 'pickerPresented' OR label == 'failed'"),
            object: app.staticTexts["pairing.state"])
        XCTAssertEqual(XCTWaiter.wait(for: [presented], timeout: 5), .completed)
        XCTAssertEqual(label(app, "pairing.state"), "pickerPresented")
        let search = try XCTUnwrap(findPickerSearchButton(app))
        let searchStartedEpoch = Date().timeIntervalSince1970
        search.element.tap()
        let searchMarker: [String: Any] = ["run_id": runID, "experiment_id": batchID,
            "source": "xctest.physical", "event": "observer_search_triggered",
            "search_started_epoch": searchStartedEpoch,
            "monotonic_ns": DispatchTime.now().uptimeNanoseconds,
            "wall_time": ISO8601DateFormatter().string(from: Date()),
            "service": service, "pairing_name": target, "device_tap_count": 0]
        let searchMarkerData = try JSONSerialization.data(withJSONObject: searchMarker, options: [.sortedKeys])
        FileHandle.standardOutput.write(Data("AWARE_OBSERVER ".utf8) + searchMarkerData + Data([10]))
        let candidates = stageATargetCandidates(target)
        let baseline = findTargetRow(app, candidates: candidates)
        let readyNS = DispatchTime.now().uptimeNanoseconds
        let marker: [String: Any] = ["run_id": runID, "experiment_id": batchID,
            "source": "xctest.physical", "event": "observer_search_requested",
            "monotonic_ns": readyNS, "wall_time": ISO8601DateFormatter().string(from: Date()),
            "service": service, "pairing_name": target,
            "baseline_row_present": baseline.row != nil, "device_tap_count": 0]
        let markerData = try JSONSerialization.data(withJSONObject: marker, options: [.sortedKeys])
        FileHandle.standardOutput.write(Data("AWARE_OBSERVER ".utf8) + markerData + Data([10]))
        var row: TargetRow?
        let observation = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
            MainActor.assumeIsolated {
                if ["failed", "invalidated", "pickerDismissed"].contains(self.label(app, "pairing.state")) { return true }
                guard baseline.row == nil else { return false }
                row = self.findTargetRow(app, candidates: candidates).row
                return row != nil
            }
        }, object: nil)
        let waitResult = XCTWaiter.wait(for: [observation], timeout: 5)
        let evidence: [String: Any] = ["run_id": runID, "experiment_id": batchID,
            "source": "xctest.physical", "test": "publisher_discovery_observation",
            "ready_monotonic_ns": readyNS, "finished_monotonic_ns": DispatchTime.now().uptimeNanoseconds,
            "baseline_row_present": baseline.row != nil, "row_appeared": row != nil,
            "row_query": row?.query ?? "", "wait_completed": waitResult == .completed,
            "device_tap_count": 0, "pin_entry_attempted": false,
            "fresh_discovery_verified": false, "state": label(app, "pairing.state")]
        let data = try JSONSerialization.data(withJSONObject: evidence, options: [.sortedKeys])
        let attachment = XCTAttachment(data: data, uniformTypeIdentifier: "public.json")
        attachment.name = runID + "-discovery-observation"
        attachment.lifetime = .keepAlways
        add(attachment)
        attachTree(app, name: runID + "-app")
        attachTree(XCUIApplication(bundleIdentifier: "com.apple.springboard"), name: runID + "-system")
    }

    @MainActor func testWiFiRuntimeReset() throws {
        continueAfterFailure = false
        let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
        settings.launch()
        defer { settings.terminate() }
        let row = settings.buttons["com.apple.settings.wifi"]
        XCTAssertTrue(row.waitForExistence(timeout: 5))
        row.tap()
        let wifiRow = settings.switches["Wi-Fi"].firstMatch
        XCTAssertTrue(wifiRow.waitForExistence(timeout: 5))
        // Settings exposes the whole row and its physical switch separately.
        let controls = settings.switches.allElementsBoundByIndex.filter {
            $0.label.isEmpty && wifiRow.frame.contains($0.frame)
                && $0.frame.width < wifiRow.frame.width / 2
        }
        XCTAssertEqual(controls.count, 1)
        let toggle = try XCTUnwrap(controls.first)
        defer { if toggle.value as? String == "0" { toggle.tap() } }
        XCTAssertEqual(toggle.value as? String, "1")
        toggle.tap()
        let off = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value == '0'"), object: toggle)
        XCTAssertEqual(XCTWaiter.wait(for: [off], timeout: 5), .completed)
        toggle.tap()
        let on = XCTNSPredicateExpectation(predicate: NSPredicate(format: "value == '1'"), object: toggle)
        XCTAssertEqual(XCTWaiter.wait(for: [on], timeout: 5), .completed)
        print("AWARE_WIFI_RUNTIME_RESET off=observed on=observed")
        fflush(stdout)
    }

    @MainActor func testCachedConnectionObservation() throws {
        continueAfterFailure = false
        let batchID = try XCTUnwrap(ProcessInfo.processInfo.environment["AWARE_BATCH_ID"])
        let deviceID = try XCTUnwrap(ProcessInfo.processInfo.environment["AWARE_PAIRED_DEVICE_ID"])
        let app = application(runID: batchID, batchID: batchID,
                              target: ProcessInfo.processInfo.environment["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName)
        app.launchEnvironment["AWARE_PAIRED_DEVICE_ID"] = deviceID
        let benchmark = ProcessInfo.processInfo.environment["AWARE_BENCH_SECONDS"].flatMap(Double.init)
        if benchmark != nil {
            app.launchEnvironment["AWARE_BENCH_SECONDS"] = ProcessInfo.processInfo.environment["AWARE_BENCH_SECONDS"]
            app.launchEnvironment["AWARE_BENCH_BYTES"] = ProcessInfo.processInfo.environment["AWARE_BENCH_BYTES"]
            app.launchEnvironment["AWARE_BENCH_TOTAL_BYTES"] =
                ProcessInfo.processInfo.environment["AWARE_BENCH_TOTAL_BYTES"]
            app.launchEnvironment["AWARE_BENCH_DIRECTIONS"] =
                ProcessInfo.processInfo.environment["AWARE_BENCH_DIRECTIONS"]
        }
        func marker(_ event: String, _ fields: [String: Any] = [:]) {
            var row: [String: Any] = ["event": event, "run_id": batchID,
                "experiment_id": batchID, "source": "xctest.physical", "paired_device_id": deviceID]
            row.merge(fields) { _, new in new }
            if let data = try? JSONSerialization.data(withJSONObject: row),
               let text = String(data: data, encoding: .utf8) {
                print("AWARE_CONNECTION " + text); fflush(stdout)
            }
        }
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["pair.refresh"].waitForExistence(timeout: 5))
        app.buttons["pair.refresh"].tap()
        let snapshot = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == 'complete' OR label == 'failed'"),
            object: app.staticTexts["pairing.snapshot"])
        XCTAssertEqual(XCTWaiter.wait(for: [snapshot], timeout: 5), .completed)
        XCTAssertEqual(app.staticTexts["pairing.snapshot"].label, "complete")
        XCTAssertTrue(parseIDs(label(app, "pairing.pairedIDs")).contains(UInt64(deviceID) ?? 0))
        marker("connection_ready")
        app.buttons["data.begin"].tap()
        marker("connection_started")
        let completion = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == 'ack_received' OR label == 'benchmark_complete' OR label == 'failed'"),
            object: app.staticTexts["data.state"])
        if let benchmark {
            // Five successful full connections took at most 21.65 s. The
            // GET 32 KiB took 11.41 s; allow setup and two transfers in 45 s.
            // Measurement ends on a completed pair after its observation span.
            let end = Date().addingTimeInterval(benchmark + 45)
            while !["benchmark_complete", "failed"].contains(label(app, "data.state")) && Date() < end {
                let terminal = XCTNSPredicateExpectation(predicate: completion.predicate,
                                                         object: app.staticTexts["data.state"])
                if XCTWaiter.wait(for: [terminal], timeout: min(5, end.timeIntervalSinceNow)) == .completed { break }
            }
        } else {
            // Discovery and connection each have a 10-second deadline;
            // allow two more seconds for the UI to observe the terminal state.
            XCTAssertEqual(XCTWaiter.wait(for: [completion], timeout: 22), .completed)
        }
        marker("connection_observed", ["state": label(app, "data.state"),
                                       "error": label(app, "data.error")])
    }

    @MainActor func testNativeFirstPacket() throws {
        continueAfterFailure = false
        let batchID = try XCTUnwrap(ProcessInfo.processInfo.environment["AWARE_BATCH_ID"])
        let app = application(runID: batchID + "-01", batchID: batchID,
                              target: ProcessInfo.processInfo.environment["AWARE_TARGET_NAME"] ?? TestConfiguration.targetName)
        app.launch()
        defer { app.terminate() }
        XCTAssertTrue(app.buttons["pair.refresh"].waitForExistence(timeout: 5))
        app.buttons["pair.refresh"].tap()
        let snapshot = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == 'complete' OR label == 'failed'"),
            object: app.staticTexts["pairing.snapshot"])
        XCTAssertEqual(XCTWaiter.wait(for: [snapshot], timeout: 5), .completed)
        XCTAssertEqual(app.staticTexts["pairing.snapshot"].label, "complete")
        app.buttons["data.begin"].tap()
        let completion = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "label == 'ack_received' OR label == 'failed'"),
            object: app.staticTexts["data.state"])
        // Discovery and connection each have a 10-second deadline.
        // Allow the UI to observe the resulting terminal state.
        XCTAssertEqual(XCTWaiter.wait(for: [completion], timeout: 22), .completed)
        XCTAssertEqual(app.staticTexts["data.state"].label, "ack_received")
    }

    @MainActor private func application(runID: String, batchID: String,
                                        target: String) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchEnvironment["AWARE_RUN_ID"] = runID
        app.launchEnvironment["AWARE_EXPERIMENT_ID"] = batchID
        app.launchEnvironment["AWARE_VENDOR_NAME"] = TestConfiguration.vendorName
        app.launchEnvironment["AWARE_MODEL_NAME"] = TestConfiguration.modelName
        app.launchEnvironment["AWARE_DISPLAY_ITEM_NAME"] = TestConfiguration.displayItemName
        app.launchEnvironment["AWARE_MODE"] = "m5"
        app.launchEnvironment["AWARE_TARGET_NAME"] = target
        app.launchEnvironment["AWARE_DISCOVERY_TIMEOUT"] = "5"
        return app
    }

    private func targetCandidates(_ target: String) -> [String] {
        var seen: [String] = []
        for value in [target, service, displayItemName, vendorName, modelName]
        where !value.isEmpty && !seen.contains(value) {
            seen.append(value)
        }
        return seen
    }

    private func stageATargetCandidates(_ target: String) -> [String] {
        var seen: [String] = []
        for value in [target, service] where !value.isEmpty && !seen.contains(value) {
            seen.append(value)
        }
        return seen
    }

    @MainActor private func label(_ app: XCUIApplication, _ identifier: String) -> String {
        let element = app.staticTexts[identifier]
        return element.exists ? element.label : ""
    }

    @MainActor private func matchCount(_ app: XCUIApplication, containing text: String) -> Int {
        let predicate = NSPredicate(format: "label CONTAINS %@", text)
        return app.staticTexts.matching(predicate).count
            + app.cells.matching(predicate).count
            + app.buttons.matching(predicate).count
    }

    private func parseIDs(_ text: String) -> [UInt64] {
        text.split(separator: ",").compactMap { UInt64($0.trimmingCharacters(in: .whitespaces)) }
    }

    @MainActor private func findTargetRow(_ app: XCUIApplication,
                                          candidates: [String]) -> (row: TargetRow?, seen: Bool) {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let roots: [(String, XCUIApplication)] = [("app", app), ("springboard", springboard)]
        var seen = false
        for (rootName, root) in roots {
            for candidate in candidates {
                let predicate = NSPredicate(format: "label CONTAINS[c] %@", candidate)
                // The App's own target label is a staticText behind the picker.
                // Only SpringBoard static texts can be a target row; in the app
                // hierarchy use cells/buttons so the App label cannot look like a
                // seen-but-not-hittable accessory.
                let queries: [(String, XCUIElementQuery)] = rootName == "app"
                    ? [("cell", root.cells), ("button", root.buttons)]
                    : [("cell", root.cells), ("button", root.buttons),
                       ("staticText", root.staticTexts)]
                for (queryName, query) in queries {
                    let matching = query.matching(predicate)
                    if matching.count > 0 { seen = true }
                    let element = matching.firstMatch
                    if element.exists && element.isHittable {
                        return (TargetRow(element: element,
                                          query: "\(rootName):\(queryName):\(candidate)"), true)
                    }
                }
            }
        }
        return (nil, seen)
    }

    @MainActor private func findPickerSearchButton(
        _ app: XCUIApplication) -> (element: XCUIElement, label: String)? {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let labels = ["查找配件", "Find Accessories", "Find Accessory",
                      "Search for Accessories", "Search Accessories"]
        for root in [app, springboard] {
            for label in labels {
                let button = root.buttons[label]
                if button.exists && button.isHittable {
                    return (button, label)
                }
            }
        }
        return nil
    }

    @MainActor private func findPINField(_ app: XCUIApplication) -> PINField? {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let roots: [(String, XCUIApplication)] = [("app", app), ("springboard", springboard)]
        for (scope, root) in roots {
            let secure = root.secureTextFields.firstMatch
            if secure.exists && secure.isHittable {
                return PINField(element: secure, scope: scope)
            }
            let text = root.textFields.firstMatch
            if text.exists && text.isHittable {
                return PINField(element: text, scope: scope)
            }
        }
        let codeTitle = springboard.alerts.textViews.matching(
            NSPredicate(format: "label IN %@", ["输入代码", "輸入代碼", "Enter Code"])).firstMatch
        if codeTitle.exists {
            for field in springboard.alerts.textViews.allElementsBoundByIndex
            where field.label.isEmpty && field.isHittable &&
                field.children(matching: .other).count == 6 {
                return PINField(element: field, scope: "springboard:ask-code")
            }
        }
        return nil
    }

    @MainActor private func submitPIN(_ field: XCUIElement, app: XCUIApplication) -> String {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let codeContinue = springboard.alerts.buttons.matching(
            NSPredicate(format: "label IN %@", ["继续", "繼續", "Continue"])).firstMatch
        if codeContinue.exists && codeContinue.isEnabled && codeContinue.isHittable {
            codeContinue.tap()
            return "button:springboard:code-continue"
        }
        let roots: [(String, XCUIApplication)] = [("app", app), ("springboard", springboard)]
        let labels = ["Pair", "OK", "Continue", "Done", "Add", "Set Up", "Finish",
                      "Confirm", "配对", "確定", "确认", "继续", "完成", "加入"]
        for (rootName, root) in roots {
            for label in labels {
                let button = root.buttons[label]
                if button.exists && button.isHittable && button.isEnabled {
                    button.tap()
                    return "button:\(rootName):\(label)"
                }
            }
        }
        field.typeText("\n")
        return "keyboardReturn"
    }

    @MainActor private func collectAlertLabels(_ app: XCUIApplication) -> [String] {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        var labels: [String] = []
        for root in [app, springboard] {
            let alerts = root.alerts
            let count = min(alerts.count, 2)
            if count == 0 { continue }
            for index in 0..<count {
                let alert = alerts.element(boundBy: index)
                if !alert.exists { continue }
                if !alert.label.isEmpty { labels.append(alert.label) }
                let texts = alert.staticTexts
                let textCount = min(texts.count, 3)
                for textIndex in 0..<textCount {
                    let text = texts.element(boundBy: textIndex).label
                    if !text.isEmpty { labels.append(text) }
                }
            }
        }
        return labels
    }

    private var pinErrorWords: [String] {
        ["incorrect", "wrong", "invalid", "try again", "not match",
         "failed", "failure", "unable", "denied", "rejected",
         "错误", "不正确", "无效", "重试", "不匹配", "失败"]
    }

    private func hasPINErrorWord(_ labels: [String]) -> Bool {
        let joined = labels.joined(separator: " ").lowercased()
        return pinErrorWords.contains { joined.contains($0) }
    }

    @MainActor private func findPINErrorText(_ app: XCUIApplication) -> String? {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        for root in [app, springboard] {
            for element in root.staticTexts.allElementsBoundByIndex.prefix(60) where element.exists {
                let text = element.label.lowercased()
                if pinErrorWords.contains(where: { text.contains($0) }) {
                    return element.label
                }
            }
        }
        return nil
    }

    private func redact(_ text: String, pin: String) -> String {
        // Redact a standalone PIN, not a six-digit substring of an ID or clock.
        guard !pin.isEmpty else { return text }
        return text.replacingOccurrences(of: "(?<![0-9])\(pin)(?![0-9])",
                                  with: "<redacted-pin>", options: .regularExpression)
    }

    @MainActor private func attachScreenshot(_ name: String) {
        let screenshot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor private func attachTree(_ app: XCUIApplication, name: String,
                                       pin: String? = nil) {
        let text = pin.map { redact(app.debugDescription, pin: $0) }
            ?? app.debugDescription
        let tree = XCTAttachment(string: text)
        tree.name = name + "-tree"
        tree.lifetime = .keepAlways
        add(tree)
    }

    @MainActor private func capture(_ app: XCUIApplication, name: String,
                                    pin: String? = nil) {
        attachScreenshot(name)
        attachTree(app, name: name, pin: pin)
    }

    @MainActor private func confirmUnpairIfPresent() {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let unpair = springboard.buttons["Unpair"]
        if unpair.waitForExistence(timeout: 5) {
            unpair.tap()
        }
    }

    @MainActor private func addResult(_ evidence: [String: Any], runID: String) {
        do {
            let data = try JSONSerialization.data(withJSONObject: evidence, options: [.sortedKeys])
            let attachment = XCTAttachment(data: data, uniformTypeIdentifier: "public.json")
            attachment.name = runID + "-m5-result"
            attachment.lifetime = .keepAlways
            add(attachment)
        } catch {
            XCTFail("M5 evidence encoding failed: \(error)")
        }
    }
}
