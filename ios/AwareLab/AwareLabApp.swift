import SwiftUI
import UIKit
import WiFiAware
import DeviceDiscoveryUI
import Network

@main
struct AwareLabApp: App {
    var body: some Scene { WindowGroup { LabView() } }
}

@MainActor @Observable
final class LabSession {
    enum State: String { case idle, ready, discoveryRequested, endpointSelected, cancelled, timedOut, failed }
    let configuration = RunConfiguration.load()
    var state: State = .idle
    var error = ""
    var capability = "unchecked"
    var showDiscovery = false
    var recorder: RunRecorder?
    var pairing: AccessoryPairingController?
    var dataConnection: NativeDataConnection?
    var timeout: Task<Void, Never>?
    private var automaticConnectionStarted = false
    init() {
        let environment = ProcessInfo.processInfo.environment
        if environment["AWARE_AUTOMATION"] == "cached_connection" ||
            environment["AWARE_MODE"] == "m5" {
            UIApplication.shared.isIdleTimerDisabled = true
        }
        do {
            recorder = try RunRecorder(configuration: configuration)
            recorder?.event("launch", state: state.rawValue)
            recorder?.result(state: state.rawValue, reason: "No wireless test has completed")
            if configuration.mode == "m4" {
                recorder?.event("m4_target_expected", state: state.rawValue, detail: "target=\(configuration.targetName); service=\(configuration.service); pairing=none")
            }
            if configuration.mode == "m5", let recorder {
                recorder.event("m5_target_expected", state: state.rawValue, detail: "target=\(configuration.targetName); service=\(configuration.service); discovery=AccessorySetupKit; role=subscriber; pin=system")
                pairing = AccessoryPairingController(configuration: configuration, recorder: recorder)
                dataConnection = NativeDataConnection(recorder: recorder)
            }
        } catch { self.error = "Record initialization: \(error)"; state = .failed }
    }
    func check() {
        let supported = WACapabilities.supportedFeatures.contains(.wifiAware)
        let declared = WASubscribableService.allServices[configuration.service] != nil
        let askWiFi = (Bundle.main.object(forInfoDictionaryKey: "NSAccessorySetupKitSupports") as? [String] ?? []).contains("WiFi")
        let askReady = configuration.mode != "m5" || askWiFi
        capability = supported ? "supported" : "unsupported"
        state = supported && declared && askReady && recorder != nil ? .ready : .failed
        if !declared { error = "Service is not declared" }
        if !supported { error = "Wi-Fi Aware unsupported" }
        if !askReady { error = "NSAccessorySetupKitSupports does not declare WiFi" }
        recorder?.event("capability_check", state: state.rawValue, detail: "wifiAware=\(supported); serviceDeclared=\(declared); askWiFi=\(askWiFi)")
    }
    func beginAutomaticConnection() {
        guard recorder?.automaticConnection == true else { return }
        check()
        guard state == .ready, let pairing else {
            recorder?.connectionMarker("connection_observed", state: "failed", error: error)
            return
        }
        pairing.refresh()
    }
    func continueAutomaticConnection() {
        guard recorder?.automaticConnection == true, !automaticConnectionStarted,
              let pairing, let dataConnection else { return }
        if pairing.snapshotState == "failed" {
            automaticConnectionStarted = true
            recorder?.connectionMarker("connection_observed", state: "failed", error: pairing.error)
        } else if pairing.snapshotState == "complete" {
            automaticConnectionStarted = true
            dataConnection.start(authorizedIDs: Set(pairing.record.accessories.filter {
                $0.state == "authorized" && $0.wifiAwarePairedDeviceID != 0
            }.map { $0.wifiAwarePairedDeviceID }))
        }
    }
    func begin() {
        guard state == .ready || state == .cancelled || state == .timedOut else { return }
        state = .discoveryRequested
        recorder?.event("discovery_requested", state: state.rawValue)
        showDiscovery = true
        timeout?.cancel()
        timeout = Task { [weak self] in
            guard let self else { return }
            do { try await Task.sleep(for: .seconds(configuration.discoveryTimeout)) } catch { return }
            guard state == .discoveryRequested else { return }
            state = .timedOut
            error = "Discovery deadline reached; no endpoint selected"
            recorder?.event("timeout", state: state.rawValue)
            recorder?.result(state: state.rawValue, reason: error)
            showDiscovery = false
        }
    }
    func selected() {
        timeout?.cancel()
        state = .endpointSelected
        recorder?.event("system_endpoint_selected", state: state.rawValue, detail: "Endpoint metadata withheld; connection not implemented")
        recorder?.result(state: state.rawValue, reason: "Endpoint selection is not a connection")
    }
    func finish() {
        timeout?.cancel()
        if state == .discoveryRequested { state = .cancelled }
        recorder?.event("discovery_container_closed", state: state.rawValue, detail: "System UI visibility must be verified by XCTest")
        recorder?.result(state: state.rawValue, reason: "No peer connection or throughput validation")
        recorder?.flush()
    }
}

struct LabView: View {
    @State private var session = LabSession()
    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    Section("Runtime") {
                        LabeledContent("Run", value: session.configuration.runID).accessibilityIdentifier("run.id")
                        Text(session.state.rawValue).accessibilityIdentifier("session.state")
                        Text(session.capability).accessibilityIdentifier("capability.value")
                        Button("Check capabilities", action: session.check).accessibilityIdentifier("capability.check")
                    }
                    if session.configuration.mode == "m4" {
                        Section("M4 discovery") {
                            LabeledContent("Mode", value: session.configuration.mode).accessibilityIdentifier("mode.value")
                            LabeledContent("Target", value: session.configuration.targetName).accessibilityIdentifier("m4.target")
                            Text("Subscriber only · discovery evidence · no pairing, no connection")
                        }
                    }
                    if session.configuration.mode == "m5", let pairing = session.pairing {
                        Section("M5 pairing") {
                            LabeledContent("Mode", value: session.configuration.mode).accessibilityIdentifier("mode.value")
                            LabeledContent("Pairing target", value: session.configuration.targetName).accessibilityIdentifier("m5.target")
                            Text(pairing.status.rawValue).accessibilityIdentifier("pairing.state")
                            Text("\(pairing.accessoryCount)").accessibilityIdentifier("pairing.accessories")
                            Text("\(pairing.authorizedCount)").accessibilityIdentifier("pairing.authorized")
                            Text(pairing.accessoryPairedDeviceIDSummary.isEmpty ? "none" : pairing.accessoryPairedDeviceIDSummary)
                                .accessibilityIdentifier("pairing.accessoryIDs")
                            Text(pairing.pairedDeviceIDSummary.isEmpty ? "none" : pairing.pairedDeviceIDSummary)
                                .accessibilityIdentifier("pairing.pairedIDs")
                            Text(pairing.snapshotState).accessibilityIdentifier("pairing.snapshot")
                            Text("Wi-Fi Aware subscriber · role subscriber · vendor \(ASKPairingDefaults.vendorName) · model \(ASKPairingDefaults.modelName) · PIN entered in the system picker")
                            Button("Pair accessory", action: pairing.start)
                                .accessibilityIdentifier("pair.begin")
                                .disabled(!pairing.canStartPicker)
                            Button("Refresh pairing state", action: pairing.refresh)
                                .accessibilityIdentifier("pair.refresh")
                            Button("Remove existing pairing", action: pairing.removeExisting)
                                .accessibilityIdentifier("pair.remove")
                                .disabled(pairing.accessoryCount == 0)
                            if let data = session.dataConnection {
                                Text(data.state).accessibilityIdentifier("data.state")
                                Button("Exchange first packet") {
                                    data.start(authorizedIDs: Set(pairing.record.accessories.filter {
                                        $0.state == "authorized" && $0.wifiAwarePairedDeviceID != 0
                                    }.map { $0.wifiAwarePairedDeviceID }))
                                }.accessibilityIdentifier("data.begin").disabled(data.state != "idle")
                                Button("Cancel connection", action: data.cancel).accessibilityIdentifier("data.cancel")
                                if !data.error.isEmpty { Text(data.error).accessibilityIdentifier("data.error") }
                            }
                            if !pairing.error.isEmpty { Text(pairing.error).accessibilityIdentifier("pairing.error") }
                        }
                    }
                    Section("Test configuration") {
                        LabeledContent("Service", value: session.configuration.service)
                        LabeledContent("Discovery timeout", value: "\(Int(session.configuration.discoveryTimeout)) s")
                        if session.configuration.mode == "m5" {
                            Text("M5 uses the AccessorySetupKit picker; the M4 DevicePicker path is disabled in this mode.")
                        } else {
                            Text("Receiver role · frontmost · no peer required for G1")
                            Button("Discover devices", action: session.begin)
                                .accessibilityIdentifier("discovery.begin")
                                .disabled(![.ready, .cancelled, .timedOut].contains(session.state))
                        }
                    }
                    Section("Result") {
                        Text("Connection and throughput are not yet validated.")
                        if !session.error.isEmpty { Text(session.error).accessibilityIdentifier("session.error") }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
            }
            .navigationTitle("Aware Lab")
            .task { session.beginAutomaticConnection() }
            .onChange(of: session.pairing?.snapshotState) { _, _ in
                session.continueAutomaticConnection()
            }
            .sheet(isPresented: $session.showDiscovery, onDismiss: session.finish) {
                NavigationStack {
                    VStack(spacing: 24) {
                        Text("Open the system picker to find a Wi-Fi Aware publisher.")
                        if let service = WASubscribableService.allServices[session.configuration.service] {
                            DevicePicker(.wifiAware(.connecting(to: .userSpecifiedDevices, from: service))) { _ in
                                session.selected()
                            } label: {
                                Text("Open system discovery")
                            } fallback: {
                                Text("System discovery unavailable").accessibilityIdentifier("discovery.unavailable")
                            }
                            .accessibilityIdentifier("discovery.system")
                        }
                        Text("No connection is inferred from opening this screen.")
                    }.padding()
                    .navigationTitle("Discovery control")
                    .toolbar { ToolbarItem(placement: .cancellationAction) {
                        Button("Finish discovery") { session.showDiscovery = false }.accessibilityIdentifier("discovery.finish")
                    } }
                }
            }
        }
    }
}
