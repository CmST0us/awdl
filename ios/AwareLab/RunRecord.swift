import Foundation
import OSLog

struct RunConfiguration: Codable {
    let runID: String
    let experimentID: String
    let directionID: String
    let discoveryTimeout: Double
    let mode: String
    let targetName: String
    let service = Bundle.main.object(forInfoDictionaryKey: "AwareFileService") as! String
    static func load(_ env: [String: String] = ProcessInfo.processInfo.environment) -> RunConfiguration {
        let requested = env["AWARE_RUN_ID"] ?? UUID().uuidString
        let safe = !requested.isEmpty && requested.count <= 100 && requested.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") }
        let seconds = Double(env["AWARE_DISCOVERY_TIMEOUT"] ?? "5") ?? 5
        let timeoutCeiling = 30.0
        let mode = env["AWARE_MODE"] == "m4" ? "m4" : env["AWARE_MODE"] == "m5" ? "m5" : "g1"
        let defaultTarget = mode == "m5" ? "ROCK5B-M5" : "ROCK5B-M4"
        let direction = mode == "m4" ? "NAN-DISCOVERY-DUAL" : mode == "m5" ? "NAN-PAIRING-DUAL" : "IOS-XCUITEST-G1"
        let requestedTarget = env["AWARE_TARGET_NAME"] ?? defaultTarget
        let targetSafe = !requestedTarget.isEmpty && requestedTarget.count <= 64 && requestedTarget.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" || $0 == ".") }
        return .init(runID: safe ? requested : UUID().uuidString, experimentID: env["AWARE_EXPERIMENT_ID"] ?? "manual", directionID: direction, discoveryTimeout: seconds.isFinite ? min(max(seconds, 2), timeoutCeiling) : 5, mode: mode, targetName: targetSafe ? requestedTarget : defaultTarget)
    }
}

struct RunEvent: Codable {
    let schemaVersion = 1
    let runID: String
    let experimentID: String
    let directionID: String
    let source = "ipad.app"
    let version: String
    let sequence: Int
    let monotonicNS: UInt64
    let wallTime: String
    let action: String
    let state: String
    let detail: String
}

/// Lifecycle logging only. A dedicated queue serializes disk I/O, never per packet.
final class RunRecorder {
    let configuration: RunConfiguration
    let directory: URL
    private let queue = DispatchQueue(label: "AwareLab.records", qos: .utility)
    private var sequence = 0
    private let logger = Logger(subsystem: "com.eric3u.rtl8852be.AwareLabRTL", category: "run")
    private let version: String
    var appVersion: String { version }
    var automaticConnection: Bool {
        ProcessInfo.processInfo.environment["AWARE_AUTOMATION"] == "cached_connection"
    }
    func connectionMarker(_ event: String, state: String = "", error: String = "") {
        guard automaticConnection else { return }
        let row: [String: String] = ["event": event, "run_id": configuration.runID,
            "experiment_id": configuration.experimentID, "source": "ipad.app",
            "paired_device_id": ProcessInfo.processInfo.environment["AWARE_PAIRED_DEVICE_ID"] ?? "",
            "state": state, "error": error]
        guard let data = try? JSONSerialization.data(withJSONObject: row),
              let text = String(data: data, encoding: .utf8) else { return }
        print("AWARE_CONNECTION " + text)
        fflush(stdout)
    }
    init(configuration: RunConfiguration, root: URL? = nil) throws {
        self.configuration = configuration
        version = "\(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") ?? "unknown")+\(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") ?? "unknown")"
        let base = root ?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Runs")
        directory = base.appendingPathComponent(configuration.runID)
        // Never append a new process to an old run: duplicate IDs are an explicit error.
        guard !FileManager.default.fileExists(atPath: directory.path) else { throw CocoaError(.fileWriteFileExists) }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try JSONEncoder().encode(configuration).write(to: directory.appendingPathComponent("configuration.json"), options: .atomic)
    }
    func event(_ action: String, state: String, detail: String = "", directionID: String? = nil) {
        let monotonic = DispatchTime.now().uptimeNanoseconds
        let wall = ISO8601DateFormatter().string(from: Date())
        logger.notice("run_id=\(self.configuration.runID, privacy: .public) action=\(action, privacy: .public) state=\(state, privacy: .public)")
        queue.async { [self] in
            sequence += 1
            let event = RunEvent(runID: configuration.runID, experimentID: configuration.experimentID, directionID: directionID ?? configuration.directionID, version: version, sequence: sequence, monotonicNS: monotonic, wallTime: wall, action: action, state: state, detail: detail)
            do {
                let url = directory.appendingPathComponent("events.jsonl")
                if !FileManager.default.fileExists(atPath: url.path) { FileManager.default.createFile(atPath: url.path, contents: nil) }
                let handle = try FileHandle(forWritingTo: url)
                defer { try? handle.close() }
                try handle.seekToEnd()
                var data = try JSONEncoder().encode(event)
                data.append(10)
                try handle.write(contentsOf: data)
                try handle.synchronize()
            } catch { logger.fault("Recording failed: \(error.localizedDescription, privacy: .public)") }
        }
    }
    func result(state: String, reason: String) {
        queue.async { [self] in
            let stage = configuration.mode == "m4" ? "M4-discovery-observation"
                : configuration.mode == "m5" ? "M5-pairing-observation" : "G1-observation"
            let result: [String: Any] = ["schemaVersion": 1, "runID": configuration.runID, "source": "ipad.app", "status": "BLOCKED", "stage": stage, "state": state, "reason": reason, "wirelessVerified": false, "throughputVerified": false]
            do { try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys]).write(to: directory.appendingPathComponent("result.json"), options: .atomic) }
            catch { logger.fault("Result write failed: \(error.localizedDescription, privacy: .public)") }
        }
    }
    func writeJSON(_ value: some Encodable, as name: String) {
        queue.async { [self] in
            do { try JSONEncoder().encode(value).write(to: directory.appendingPathComponent(name), options: .atomic) }
            catch { logger.fault("Record \(name, privacy: .public) write failed: \(error.localizedDescription, privacy: .public)") }
        }
    }
    func flush() { queue.sync {} }
}
