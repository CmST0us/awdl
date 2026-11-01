import Foundation
import Observation
import UIKit
import AccessorySetupKit
import WiFiAware

/// Publisher identity from the shared defaults, with per-run diagnostic overrides.
enum ASKPairingDefaults {
    static let vendorName = ProcessInfo.processInfo.environment["AWARE_VENDOR_NAME"] ?? Bundle.main.object(forInfoDictionaryKey: "AwareVendorName") as! String
    static let modelName = ProcessInfo.processInfo.environment["AWARE_MODEL_NAME"] ?? Bundle.main.object(forInfoDictionaryKey: "AwareModelName") as! String
    static let displayItemName = ProcessInfo.processInfo.environment["AWARE_DISPLAY_ITEM_NAME"] ?? modelName
    static let serviceRole = "subscriber"
}

/// Drives the system accessory picker in M5 mode. The app never asks for or
/// supplies a PIN: the system picker owns that interaction. Every value written
/// here comes from AccessorySetupKit or WiFiAware callbacks.
@MainActor
@Observable
final class AccessoryPairingController {
    enum Status: String {
        case idle, activating, ready, pickerRequested, pickerPresented, pickerDismissed
        case authorized, invalidated, failed
    }

    private(set) var status: Status = .idle
    private(set) var error = ""
    private(set) var snapshotState = "pending"
    private(set) var record: ASKPairingRecord

    @ObservationIgnored private let session = ASAccessorySession()
    @ObservationIgnored private let recorder: RunRecorder
    @ObservationIgnored private let service: String
    @ObservationIgnored private var activationRequested = false
    @ObservationIgnored private var pendingPicker = false
    @ObservationIgnored private var pendingSnapshot = false
    @ObservationIgnored private var snapshotGeneration = 0

    init(configuration: RunConfiguration, recorder: RunRecorder) {
        self.recorder = recorder
        self.service = configuration.service
        let technologies = Bundle.main.object(forInfoDictionaryKey: "NSAccessorySetupKitSupports") as? [String] ?? []
        record = ASKPairingRecord(
            runID: configuration.runID,
            appVersion: recorder.appVersion,
            supportedTechnologies: technologies,
            descriptor: ASKDescriptorRecord(
                wifiAwareServiceName: configuration.service,
                wifiAwareServiceRole: ASKPairingDefaults.serviceRole,
                wifiAwareVendorName: ASKPairingDefaults.vendorName,
                wifiAwareModelName: ASKPairingDefaults.modelName,
                displayItemName: ASKPairingDefaults.displayItemName,
                expectedPairingName: configuration.targetName))
        persist()
    }

    var canStartPicker: Bool { status == .idle || status == .ready }
    var accessoryCount: Int { record.accessories.count }
    var authorizedCount: Int { record.authorizedCount }
    var accessoryPairedDeviceIDSummary: String {
        record.accessories.map { String($0.wifiAwarePairedDeviceID) }.joined(separator: ",")
    }
    var pairedDeviceIDSummary: String {
        record.pairedDevices.map { String($0.id) }.joined(separator: ",")
    }

    /// Activate the session if needed and present the picker for the board.
    func start() {
        guard canStartPicker else { return }
        pendingPicker = true
        activateIfNeeded()
        if record.session.activated { refreshPairedDevices(reason: "before_picker") }
    }

    /// Read the current AccessorySetupKit and WiFiAware state without opening the picker.
    func refresh() {
        pendingSnapshot = true
        activateIfNeeded()
        guard record.session.activated else { return }
        pendingSnapshot = false
        record.appendEvent("ask_snapshot_requested",
                           detail: "Manual pairing-state refresh without the picker")
        refreshAccessories()
        refreshPairedDevices(reason: "manual_refresh")
        persist()
    }

    func removeExisting() {
        guard let accessory = session.accessories.first else {
            error = "No existing accessory to remove"
            return
        }
        error = ""
        recorder.event("ask_remove_requested", state: status.rawValue,
                       detail: "displayName=\(accessory.displayName)")
        session.removeAccessory(accessory) { [weak self] error in
            Task { @MainActor in
                guard let self else { return }
                if let error {
                    let text = String(describing: error)
                    self.error = "ASK remove: \(text)"
                    self.recorder.event("ask_remove_failed", state: self.status.rawValue,
                                        detail: text)
                    self.status = .failed
                } else {
                    self.recorder.event("ask_remove_completed", state: self.status.rawValue,
                                        detail: "displayName=\(accessory.displayName)")
                    self.refreshAccessories()
                    self.status = self.accessoryStatus()
                    self.refreshPairedDevices(reason: "manual_remove")
                }
                self.persist()
            }
        }
    }

    private func activateIfNeeded() {
        guard !activationRequested else { return }
        activationRequested = true
        status = .activating
        record.markActivationRequested()
        persist()
        session.activate(on: .main) { [weak self] event in
            MainActor.assumeIsolated { self?.handle(event) }
        }
    }

    private func showPicker() {
        guard !record.session.pickerRequested else { return }
        // Keep the system default picker timeout (about 30 s); longer windows
        // require a separate explicit, user-approved change.
        let descriptor = ASDiscoveryDescriptor()
        descriptor.wifiAwareServiceName = service
        descriptor.wifiAwareServiceRole = .subscriber
        descriptor.wifiAwareVendorNameMatch = ASPropertyCompareString(
            string: ASKPairingDefaults.vendorName, compareOptions: [])
        descriptor.wifiAwareModelNameMatch = ASPropertyCompareString(
            string: ASKPairingDefaults.modelName, compareOptions: [])
        let item = ASPickerDisplayItem(name: ASKPairingDefaults.displayItemName,
                                       productImage: Self.productImage,
                                       descriptor: descriptor)
        record.markPickerRequested()
        status = .pickerRequested
        persist()
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                try await self.session.showPicker(for: [item])
                self.record.markPickerCompletion(error: nil)
            } catch {
                let text = String(describing: error)
                self.error = "ASK picker: \(text)"
                self.record.markPickerCompletion(error: text)
                if !self.record.session.pickerPresented { self.status = .failed }
            }
            self.refreshAccessories()
            self.refreshPairedDevices(reason: "picker_completed")
            if self.record.authorizedCount > 0 { self.status = .authorized }
            self.persist()
        }
    }

    private func handle(_ event: ASAccessoryEvent) {
        let accessory = event.accessory.map(Self.snapshot)
        let eventError = event.error.map { String(describing: $0) }
        switch event.eventType {
        case .activated:
            status = .ready
            record.markActivated()
            refreshAccessories()
            refreshPairedDevices(reason: "session_activated")
            if pendingSnapshot {
                pendingSnapshot = false
                refreshPairedDevices(reason: "manual_refresh")
            }
        case .invalidated:
            record.markInvalidated()
            status = .invalidated
        case .pickerDidPresent:
            record.markPickerPresented()
            status = .pickerPresented
        case .pickerDidDismiss:
            record.markPickerDismissed()
            refreshAccessories()
            status = record.authorizedCount > 0 ? .authorized : .pickerDismissed
            refreshPairedDevices(reason: "picker_dismissed")
        case .accessoryAdded:
            record.markAccessoryEvent(kind: "added", accessory: accessory, error: eventError)
            refreshAccessories()
            status = accessoryStatus()
            refreshPairedDevices(reason: "accessory_added")
        case .accessoryChanged:
            record.markAccessoryEvent(kind: "changed", accessory: accessory, error: eventError)
            refreshAccessories()
            status = accessoryStatus()
            refreshPairedDevices(reason: "accessory_changed")
        case .accessoryRemoved:
            record.markAccessoryEvent(kind: "removed", accessory: accessory, error: eventError)
            refreshAccessories()
            status = accessoryStatus()
            refreshPairedDevices(reason: "accessory_removed")
        default:
            record.markGenericEvent(name: Self.name(for: event.eventType),
                                    code: event.eventType.rawValue,
                                    accessory: accessory, error: eventError)
        }
        if let eventError, !eventError.isEmpty {
            error = "ASK \(Self.name(for: event.eventType)): \(eventError)"
        }
        persist()
    }

    private func accessoryStatus() -> Status {
        if record.authorizedCount > 0 { return .authorized }
        if record.session.pickerPresented { return .pickerPresented }
        return .ready
    }

    private func refreshAccessories() {
        record.setAccessories(session.accessories.map(Self.snapshot))
    }

    private func refreshPairedDevices(reason: String) {
        snapshotGeneration += 1
        let generation = snapshotGeneration
        snapshotState = "pending"
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let devices = try await WAPairedDevice.allDevices.current() ?? [:]
                guard generation == self.snapshotGeneration else { return }
                let snapshots = devices.values.map { device in
                    ASKPairedDeviceSnapshot(id: device.id, name: device.name,
                                            pairingName: device.pairingInfo?.pairingName,
                                            vendorName: device.pairingInfo?.vendorName,
                                            modelName: device.pairingInfo?.modelName)
                }
                self.record.setPairedDevices(snapshots, error: nil, reason: reason)
                self.snapshotState = "complete"
                if self.pendingPicker {
                    self.pendingPicker = false
                    self.showPicker()
                }
            } catch {
                guard generation == self.snapshotGeneration else { return }
                self.record.setPairedDevices([], error: String(describing: error), reason: reason)
                self.snapshotState = "failed"
                if self.pendingPicker {
                    self.pendingPicker = false
                    self.status = .failed
                    self.error = "Pairing baseline unavailable"
                }
            }
            self.persist()
        }
    }

    private func persist() {
        recorder.writeJSON(record, as: "ask-session.json")
        recorder.result(state: record.outcome,
                        reason: "ASK observation only; the app does not certify system pairing")
        recorder.flush()
    }

    private static func snapshot(_ accessory: ASAccessory) -> ASKAccessorySnapshot {
        ASKAccessorySnapshot(displayName: accessory.displayName,
                             state: stateName(accessory.state),
                             wifiAwarePairedDeviceID: accessory.wifiAwarePairedDeviceID,
                             hasSSID: accessory.ssid != nil,
                             hasBluetoothIdentifier: accessory.bluetoothIdentifier != nil)
    }

    private static func stateName(_ state: ASAccessory.AccessoryState) -> String {
        switch state {
        case .unauthorized: return "unauthorized"
        case .awaitingAuthorization: return "awaitingAuthorization"
        case .authorized: return "authorized"
        @unknown default: return "unknown"
        }
    }

    private static func name(for type: ASAccessoryEventType) -> String {
        switch type {
        case .activated: return "activated"
        case .invalidated: return "invalidated"
        case .migrationComplete: return "migration_complete"
        case .accessoryAdded: return "accessory_added"
        case .accessoryRemoved: return "accessory_removed"
        case .accessoryChanged: return "accessory_changed"
        case .accessoryDiscovered: return "accessory_discovered"
        case .pickerDidPresent: return "picker_did_present"
        case .pickerDidDismiss: return "picker_did_dismiss"
        case .pickerSetupBridging: return "picker_setup_bridging"
        case .pickerSetupFailed: return "picker_setup_failed"
        case .pickerSetupPairing: return "picker_setup_pairing"
        case .pickerSetupRename: return "picker_setup_rename"
        case .unknown: return "unknown"
        @unknown default: return "unknown_\(type.rawValue)"
        }
    }

    private static var productImage: UIImage {
        UIImage(systemName: "wifi.router")
            ?? UIGraphicsImageRenderer(size: CGSize(width: 64, height: 64)).image { _ in }
    }
}
