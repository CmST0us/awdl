import Foundation
import Dispatch

/// Versioned AccessorySetupKit observation record for the M5 pairing mode.
/// The record contains only identifiers the system already exposes to the app;
/// no PIN, key or pairing secret is read or written here.
struct ASKDescriptorRecord: Codable, Equatable {
    let wifiAwareServiceName: String
    let wifiAwareServiceRole: String
    let wifiAwareVendorName: String
    let wifiAwareModelName: String
    let displayItemName: String
    let expectedPairingName: String
}

struct ASKAccessorySnapshot: Codable, Equatable {
    let displayName: String
    let state: String
    let wifiAwarePairedDeviceID: UInt64
    let hasSSID: Bool
    let hasBluetoothIdentifier: Bool
}

struct ASKPairedDeviceSnapshot: Codable, Equatable {
    let id: UInt64
    let name: String?
    let pairingName: String?
    let vendorName: String?
    let modelName: String?
}

struct ASKLogEvent: Codable, Equatable {
    let sequence: Int
    let monotonicNS: UInt64
    let wallTime: String
    let event: String
    let eventCode: Int?
    let detail: String
    let accessory: ASKAccessorySnapshot?
    let error: String?
}

struct ASKSessionState: Codable, Equatable {
    var activated = false
    var activationMonotonicNS: UInt64?
    var invalidated = false
    var invalidationMonotonicNS: UInt64?
    var pickerRequested = false
    var pickerRequestedMonotonicNS: UInt64?
    var pickerPresented = false
    var pickerPresentedMonotonicNS: UInt64?
    var pickerDismissed = false
    var pickerDismissedMonotonicNS: UInt64?
    var pickerCompletionError: String?
    var accessoryAddedCount = 0
    var accessoryChangedCount = 0
    var accessoryRemovedCount = 0
}

enum ASKOutcome: String {
    case notStarted = "not_started"
    case sessionReady = "session_ready"
    case pickerRequested = "picker_requested"
    case pickerPresented = "picker_presented"
    case pickerFailed = "picker_failed"
    case pickerDismissedWithoutAccessory = "picker_dismissed_without_accessory"
    case pickerDismissedWithAccessory = "picker_dismissed_with_accessory"
    case accessoryObserved = "accessory_observed"
    case authorized = "authorized"
}

struct ASKPairingRecord: Codable, Equatable {
    static let directionID = "NAN-PAIRING-DUAL"
    static let pinHandling = "system_picker"

    let schemaVersion: Int
    let runID: String
    let source: String
    let directionID: String
    let appVersion: String
    let supportedTechnologies: [String]
    let descriptor: ASKDescriptorRecord
    let pinHandling: String
    var session = ASKSessionState()
    var accessories: [ASKAccessorySnapshot] = []
    var pairedDevices: [ASKPairedDeviceSnapshot] = []
    var initialAccessories: [ASKAccessorySnapshot]?
    var initialPairedDevices: [ASKPairedDeviceSnapshot]?
    var pairedDeviceSnapshotError: String?
    var outcome: String = ASKOutcome.notStarted.rawValue
    var events: [ASKLogEvent] = []

    init(runID: String, appVersion: String, supportedTechnologies: [String],
         descriptor: ASKDescriptorRecord) {
        self.schemaVersion = 1
        self.runID = runID
        self.source = "ipad.app"
        self.directionID = ASKPairingRecord.directionID
        self.pinHandling = ASKPairingRecord.pinHandling
        self.appVersion = appVersion
        self.supportedTechnologies = supportedTechnologies
        self.descriptor = descriptor
    }

    var authorizedCount: Int { accessories.filter { $0.state == "authorized" }.count }

    /// The outcome is a record of what the app observed, not a pairing verdict.
    /// A nil completion error plus a dismissed picker means the picker finished;
    /// the app cannot tell a person's cancellation from a discovery timeout.
    static func deriveOutcome(accessories: [ASKAccessorySnapshot],
                              session: ASKSessionState) -> ASKOutcome {
        if accessories.contains(where: { $0.state == "authorized" }) { return .authorized }
        if session.pickerCompletionError != nil { return .pickerFailed }
        if session.pickerDismissed {
            return session.accessoryAddedCount > 0
                ? .pickerDismissedWithAccessory : .pickerDismissedWithoutAccessory
        }
        if !accessories.isEmpty { return .accessoryObserved }
        if session.pickerPresented { return .pickerPresented }
        if session.pickerRequested { return .pickerRequested }
        if session.activated { return .sessionReady }
        return .notStarted
    }

    mutating func appendEvent(_ event: String, code: Int? = nil, detail: String = "",
                              accessory: ASKAccessorySnapshot? = nil, error: String? = nil,
                              monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                              wallTime: String = ASKPairingRecord.wallTime()) {
        events.append(ASKLogEvent(sequence: events.count + 1, monotonicNS: monotonicNS,
                                  wallTime: wallTime, event: event, eventCode: code,
                                  detail: detail, accessory: accessory, error: error))
    }

    mutating func markActivationRequested(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                          wallTime: String = ASKPairingRecord.wallTime()) {
        appendEvent("ask_activation_requested", detail: "AccessorySetupKit session activation requested",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markActivated(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                wallTime: String = ASKPairingRecord.wallTime()) {
        session.activated = true
        session.activationMonotonicNS = monotonicNS
        appendEvent("ask_session_activated", detail: "AccessorySetupKit session activated",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markInvalidated(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                  wallTime: String = ASKPairingRecord.wallTime()) {
        session.invalidated = true
        session.invalidationMonotonicNS = monotonicNS
        appendEvent("ask_session_invalidated", detail: "AccessorySetupKit session invalidated",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markPickerRequested(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                      wallTime: String = ASKPairingRecord.wallTime()) {
        session.pickerRequested = true
        session.pickerRequestedMonotonicNS = monotonicNS
        appendEvent("ask_picker_requested",
                    detail: "showPicker(for:) called with the Wi-Fi Aware subscriber descriptor",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markPickerPresented(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                      wallTime: String = ASKPairingRecord.wallTime()) {
        session.pickerPresented = true
        session.pickerPresentedMonotonicNS = monotonicNS
        appendEvent("ask_picker_presented", detail: "System accessory picker presented",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markPickerDismissed(monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                      wallTime: String = ASKPairingRecord.wallTime()) {
        session.pickerDismissed = true
        session.pickerDismissedMonotonicNS = monotonicNS
        appendEvent("ask_picker_dismissed", detail: "System accessory picker dismissed",
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markPickerCompletion(error: String?,
                                       monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                       wallTime: String = ASKPairingRecord.wallTime()) {
        session.pickerCompletionError = error
        appendEvent("ask_picker_completion",
                    detail: error == nil ? "Picker completed without a framework error"
                                         : "Picker completed with a framework error",
                    error: error, monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markAccessoryEvent(kind: String, accessory: ASKAccessorySnapshot?,
                                     error: String?,
                                     monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                     wallTime: String = ASKPairingRecord.wallTime()) {
        switch kind {
        case "added": session.accessoryAddedCount += 1
        case "changed": session.accessoryChangedCount += 1
        case "removed": session.accessoryRemovedCount += 1
        default: break
        }
        appendEvent("ask_accessory_\(kind)", detail: accessory.map { "state=\($0.state)" } ?? "",
                    accessory: accessory, error: error,
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func markGenericEvent(name: String, code: Int?, accessory: ASKAccessorySnapshot?,
                                   error: String?,
                                   monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                   wallTime: String = ASKPairingRecord.wallTime()) {
        appendEvent("ask_\(name)", code: code, accessory: accessory, error: error,
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func setAccessories(_ snapshots: [ASKAccessorySnapshot]) {
        accessories = snapshots
        if initialAccessories == nil { initialAccessories = snapshots }
        refreshOutcome()
    }

    mutating func setPairedDevices(_ snapshots: [ASKPairedDeviceSnapshot], error: String?,
                                   reason: String,
                                   monotonicNS: UInt64 = DispatchTime.now().uptimeNanoseconds,
                                   wallTime: String = ASKPairingRecord.wallTime()) {
        pairedDevices = snapshots.sorted { $0.id < $1.id }
        if initialPairedDevices == nil && error == nil { initialPairedDevices = pairedDevices }
        pairedDeviceSnapshotError = error
        appendEvent("ask_paired_device_snapshot",
                    detail: "\(reason); devices=\(snapshots.count)", error: error,
                    monotonicNS: monotonicNS, wallTime: wallTime)
        refreshOutcome()
    }

    mutating func refreshOutcome() {
        outcome = ASKPairingRecord.deriveOutcome(accessories: accessories, session: session).rawValue
    }

    static func wallTime() -> String { ISO8601DateFormatter().string(from: Date()) }
}
