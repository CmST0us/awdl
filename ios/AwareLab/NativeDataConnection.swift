import Foundation
import Darwin
import Observation
import Network
import WiFiAware

struct BulkBenchmarkPlan {
    let blockBytes: Int
    let totalBytes: Int
    let directions: [String]

    init?(blockBytes: Int, seconds: Double, totalBytes: Int, directions: String) {
        guard (1...16*1024*1024).contains(blockBytes), (0...300).contains(seconds),
              totalBytes >= 0, ["BOTH", "GET", "PUT"].contains(directions),
              totalBytes == 0 || seconds > 0 else { return nil }
        self.blockBytes = blockBytes
        self.totalBytes = totalBytes
        self.directions = directions == "BOTH" ? ["GET", "PUT"] : [directions]
    }

    func nextBlock(direction: String, transferred: Int) -> Int? {
        guard directions.contains(direction) else { return nil }
        guard totalBytes > 0 else { return blockBytes }
        let remaining = totalBytes-transferred
        return remaining > 0 ? min(blockBytes, remaining) : nil
    }

    func complete(downBytes: Int, upBytes: Int) -> Bool {
        guard totalBytes > 0 else { return false }
        return directions.allSatisfy {
            ($0 == "GET" ? downBytes : upBytes) >= totalBytes
        }
    }
}

struct BulkReadTiming {
    private var previous: Double
    private(set) var completions = 0
    private(set) var maximumGap = 0.0

    init(started: Double) { previous = started }

    mutating func received(at now: Double) {
        maximumGap = max(maximumGap, now - previous)
        previous = now
        completions += 1
    }
}

@MainActor @Observable
final class NativeDataConnection {
    private(set) var state = "idle"
    private(set) var error = ""
    @ObservationIgnored private let recorder: RunRecorder
    @ObservationIgnored private var browser: NWBrowser?
    @ObservationIgnored private var connection: NWConnection?
    @ObservationIgnored private var deadline: Task<Void, Never>?
    @ObservationIgnored private var setup: Task<Void, Never>?
    @ObservationIgnored private var exchange: FirstPacketExchange
    @ObservationIgnored private var selectedID: UInt64?

    init(recorder: RunRecorder) {
        self.recorder = recorder
        exchange = FirstPacketExchange(runID: recorder.configuration.runID)
    }

    func start(authorizedIDs: Set<UInt64>) {
        guard state == "idle" else { return }
        guard !authorizedIDs.isEmpty else { fail("No authorized Wi-Fi Aware device ID"); return }
        transition("resolving", "service=\(recorder.configuration.service)")
        // Cached path setup measured 2.09-4.17 seconds; leave scan callback
        // variance inside the same 10-second bound used by connect.
        armDeadline(seconds: 10)
        setup = Task { [weak self] in
            guard let self else { return }
            do {
                let all = try await WAPairedDevice.allDevices.current() ?? [:]
                guard !Task.isCancelled else { return }
                let requestedID = ProcessInfo.processInfo.environment["AWARE_PAIRED_DEVICE_ID"]
                    .flatMap(UInt64.init)
                let devices = all.values.filter {
                    authorizedIDs.contains($0.id) && $0.id != 0
                    && (requestedID == nil || $0.id == requestedID)
                    && $0.pairingInfo?.pairingName == recorder.configuration.targetName
                }
                guard devices.count == 1, let device = devices.first,
                      let service = WASubscribableService.allServices[recorder.configuration.service] else {
                    fail("Expected exactly one authorized target in WAPairedDevice"); return
                }
                selectedID = device.id
                let provider = WASubscriberBrowser.wifiAware(.connecting(to: .selected([device]), from: service))
                let parameters = provider.configureParameters(.tcp)
                // Request the bulk performance mode so iOS negotiates the wide
                // 5GHz/80MHz data path instead of the conservative 20MHz default.
                parameters.wifiAware = WAParameters(performanceMode: .bulk)
                let browser = NWBrowser(for: provider.makeDescriptor(), using: parameters)
                self.browser = browser
                browser.stateUpdateHandler = { [weak self] state in
                    MainActor.assumeIsolated {
                        if case .failed(let error) = state { self?.fail("Browser: \(error)") }
                    }
                }
                browser.browseResultsChangedHandler = { [weak self] results, _ in
                    MainActor.assumeIsolated {
                        guard let self, self.connection == nil, self.state == "browsing" else { return }
                        do {
                            for result in results {
                                guard let endpoint = try provider.makeEndpoint(from: result),
                                      endpoint.device.id == device.id else { continue }
                                self.connect(result.endpoint, parameters: parameters)
                                break
                            }
                        } catch { self.fail("Endpoint: \(error)") }
                    }
                }
                transition("browsing", "pairedDeviceID=\(device.id)")
                browser.start(queue: .main)
            } catch { fail("Paired devices: \(error)") }
        }
    }

    private func connect(_ endpoint: NWEndpoint, parameters: NWParameters) {
        transition("connecting", "endpoint=\(endpoint); pairedDeviceID=\(selectedID ?? 0)")
        // Cached PASN, NDP and TCP readiness measured 2.09–4.17 seconds.
        armDeadline(seconds: 10)
        let connection = NWConnection(to: endpoint, using: parameters)
        self.connection = connection
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            MainActor.assumeIsolated {
                guard let self, let connection, self.connection === connection else { return }
                switch state {
                case .ready: self.verifyPath(connection)
                case .failed(let error): self.fail("Connection: \(error)")
                case .waiting(let error): self.transition("waiting", "\(error)")
                default: break
                }
            }
        }
        connection.start(queue: .main)
    }

    private func verifyPath(_ connection: NWConnection) {
        guard state != "verifying_path" && state != "receiving" else { return }
        transition("verifying_path")
        setup = Task { [weak self, weak connection] in
            guard let self, let connection else { return }
            do {
                guard let path = connection.currentPath,
                      let aware = try await path.wifiAware,
                      aware.endpoint.device.id == selectedID else {
                    fail("Ready connection has no matching Wi-Fi Aware path"); return
                }
                guard self.connection === connection, !Task.isCancelled else { return }
                transition("native_path_ready", "pairedDeviceID=\(aware.endpoint.device.id); interfaces=\(path.availableInterfaces.map { "\($0.name):\($0.index)" }.joined(separator: ",")); local=\(String(describing: path.localEndpoint)); remote=\(String(describing: path.remoteEndpoint))")
                browser?.cancel()
                browser = nil
                armDeadline()
                if let bytes = ProcessInfo.processInfo.environment["AWARE_BENCH_BYTES"].flatMap(Int.init),
                   let seconds = ProcessInfo.processInfo.environment["AWARE_BENCH_SECONDS"].flatMap(Double.init) {
                    let totalBytes = ProcessInfo.processInfo.environment["AWARE_BENCH_TOTAL_BYTES"]
                        .flatMap(Int.init) ?? 0
                    let directions = ProcessInfo.processInfo.environment["AWARE_BENCH_DIRECTIONS"] ?? "BOTH"
                    guard let plan = BulkBenchmarkPlan(blockBytes: bytes, seconds: seconds,
                                                       totalBytes: totalBytes, directions: directions) else {
                        throw BulkError.invalidResponse
                    }
                    try await benchmark(connection, plan: plan, seconds: seconds)
                    return
                }
                transition("receiving")
                receive(connection)
                connection.send(content: exchange.request, completion: .contentProcessed { [weak self] error in
                    MainActor.assumeIsolated {
                        guard let self, self.connection === connection else { return }
                        if let error { self.fail("Send: \(error)"); return }
                        self.recorder.event("first_packet_send_completed", state: self.state,
                                            detail: "messageID=1; bytes=\(self.exchange.request.count)", directionID: "M5-NAN-DATA")
                    }
                })
            } catch {
                guard self.connection === connection else { return }
                fail("Native transfer: \(error)")
            }
        }
    }

    private func receive(_ connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 257) { [weak self] data, _, complete, error in
            MainActor.assumeIsolated {
                guard let self, self.connection === connection else { return }
                do {
                    if try self.exchange.consume(data ?? Data(), complete: complete) {
                        self.transition("ack_received", "messageID=1; bytes=\(self.exchange.received.count); pairedDeviceID=\(self.selectedID ?? 0)")
                        self.recorder.result(state: self.state, reason: "Matching ACK received on Wi-Fi Aware; board receipt evidence required")
                        self.stopTransport()
                    } else if let error { self.fail("Receive: \(error)") }
                    else { self.receive(connection) }
                } catch { self.fail("Response: \(error)") }
            }
        }
    }

    private enum BulkError: Error { case invalidResponse, closed, contentMismatch, targetNotMet }

    private func bulkSend(_ data: Data, on connection: NWConnection) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: data, completion: .contentProcessed { error in
                if let error { continuation.resume(throwing: error) }
                else { continuation.resume() }
            })
        }
        try Task.checkCancellation()
        armDeadline()
    }

    private static let bulkChunkSize = 256 * 1024
    private static let bulkPattern = Data((0..<(bulkChunkSize + 250)).map { UInt8($0 % 251) })

    private func bulkRead(_ count: Int, minimum: Int = 1, on connection: NWConnection) async throws -> Data {
        let data: Data = try await withCheckedThrowingContinuation { continuation in
            connection.receive(minimumIncompleteLength: minimum, maximumLength: count) { data, _, _, error in
                if let data, !data.isEmpty { continuation.resume(returning: data) }
                else { continuation.resume(throwing: error.map { $0 as Error } ?? BulkError.closed) }
            }
        }
        try Task.checkCancellation()
        armDeadline()
        return data
    }

    private func bulkLine(on connection: NWConnection) async throws -> String {
        var data = Data()
        while data.count < 256 {
            let byte = try await bulkRead(1, on: connection)
            if byte.first == 10 {
                guard let line = String(data: data, encoding: .utf8) else { throw BulkError.invalidResponse }
                return line
            }
            data.append(byte)
        }
        throw BulkError.invalidResponse
    }

    private func bulkUpload(_ bytes: Int, on connection: NWConnection) async throws {
        var offset = 0
        while offset < bytes {
            let count = min(Self.bulkChunkSize, bytes-offset)
            let start = offset % 251
            try await bulkSend(Self.bulkPattern.subdata(in: start..<(start+count)), on: connection)
            offset += count
        }
    }

    private func bulkPut(_ bytes: Int, sequence: Int, on connection: NWConnection) async throws -> Double {
        async let upload: Void = bulkUpload(bytes, on: connection)
        var received = 0
        while true {
            let fields = try await bulkLine(on: connection).split(separator: " ").map(String.init)
            guard fields.count >= 4, fields[1] == recorder.configuration.runID,
                  fields[2] == String(sequence), let count = Int(fields[3]),
                  count >= received, count <= bytes else { throw BulkError.invalidResponse }
            if fields[0] == "PROGRESS", fields.count == 4 {
                received = count
                recorder.event("bulk_remote_progress", state: state,
                               detail: "sequence=\(sequence); bytes=\(count)")
            } else if fields[0] == "DONE", fields.count == 5, count == bytes,
                      let microseconds = Double(fields[4]), microseconds > 0 {
                try await upload
                return microseconds/1_000_000
            } else { throw BulkError.invalidResponse }
        }
    }

    private func benchmark(_ connection: NWConnection, plan: BulkBenchmarkPlan,
                           seconds: Double) async throws {
        transition("benchmark_running",
                   "blockBytes=\(plan.blockBytes); totalBytes=\(plan.totalBytes); directions=\(plan.directions.joined(separator: ",")); seconds=\(seconds)")
        _ = Self.bulkPattern
        let begin = ProcessInfo.processInfo.systemUptime
        let run = recorder.configuration.runID
        var sequence = 0
        var downBytes = 0, upBytes = 0
        var downSeconds = 0.0, upSeconds = 0.0
        var downWallSeconds = 0.0, upWallSeconds = 0.0
        repeat {
            for direction in plan.directions {
                let transferred = direction == "GET" ? downBytes : upBytes
                guard let bytes = plan.nextBlock(direction: direction, transferred: transferred) else {
                    continue
                }
                sequence += 1
                let started = ProcessInfo.processInfo.systemUptime
                recorder.event("bulk_begin", state: state,
                               detail: "sequence=\(sequence); direction=\(direction); bytes=\(bytes)")
                try await bulkSend(Data("BULK \(run) \(sequence) \(direction) \(bytes)\n".utf8), on: connection)
                guard try await bulkLine(on: connection) == "READY \(run) \(sequence) \(bytes)" else {
                    throw BulkError.invalidResponse
                }
                let elapsed: Double
                var readTiming = BulkReadTiming(started: started)
                if direction == "PUT" {
                    elapsed = try await bulkPut(bytes, sequence: sequence, on: connection)
                    upBytes += bytes; upSeconds += elapsed
                } else {
                    var offset = 0
                    var reported = started
                    while offset < bytes {
                        let count = min(Self.bulkChunkSize, bytes-offset)
                        let data = try await bulkRead(count, minimum: count, on: connection)
                        readTiming.received(at: ProcessInfo.processInfo.systemUptime)
                        let equal = data.withUnsafeBytes { actual in
                            Self.bulkPattern.withUnsafeBytes { expected in
                                memcmp(actual.baseAddress!, expected.baseAddress! + offset % 251, data.count) == 0
                            }
                        }
                        guard equal else { throw BulkError.contentMismatch }
                        offset += data.count
                        let now = ProcessInfo.processInfo.systemUptime
                        if now-reported >= 1 || offset == bytes {
                            recorder.event("bulk_receive_progress", state: state,
                                           detail: "sequence=\(sequence); bytes=\(offset); elapsed=\(now-started)")
                            reported = now
                        }
                    }
                    elapsed = ProcessInfo.processInfo.systemUptime-started
                    downBytes += bytes; downSeconds += elapsed
                }
                let wallSeconds = ProcessInfo.processInfo.systemUptime-started
                if direction == "GET" { downWallSeconds += wallSeconds }
                else { upWallSeconds += wallSeconds }
                recorder.event("bulk_receiver_complete", state: state,
                               detail: "sequence=\(sequence); direction=\(direction); bytes=\(bytes); seconds=\(elapsed); MBps=\(Double(bytes)/elapsed/1_000_000); readCompletions=\(readTiming.completions); maxReadCompletionGapSeconds=\(readTiming.maximumGap)")
            }
            let elapsed = ProcessInfo.processInfo.systemUptime-begin
            if plan.complete(downBytes: downBytes, upBytes: upBytes) {
                break
            }
            if elapsed >= seconds {
                if plan.totalBytes > 0 {
                    throw BulkError.targetNotMet
                }
                break
            }
        } while true
        let elapsed = ProcessInfo.processInfo.systemUptime-begin
        struct Summary: Encodable {
            let run_id: String
            let seconds: Double
            let transfers, down_bytes, up_bytes: Int
            let target_bytes: Int
            let directions: String
            let down_receiver_seconds, up_receiver_seconds, down_MBps, up_MBps: Double
            let down_end_to_end_seconds, up_end_to_end_seconds, down_end_to_end_MBps, up_end_to_end_MBps: Double
        }
        recorder.writeJSON(Summary(run_id: run, seconds: elapsed, transfers: sequence,
                                   down_bytes: downBytes, up_bytes: upBytes,
                                   target_bytes: plan.totalBytes,
                                   directions: plan.directions.joined(separator: ","),
                                   down_receiver_seconds: downSeconds, up_receiver_seconds: upSeconds,
                                   down_MBps: downSeconds > 0 ? Double(downBytes)/downSeconds/1_000_000 : 0,
                                   up_MBps: upSeconds > 0 ? Double(upBytes)/upSeconds/1_000_000 : 0,
                                   down_end_to_end_seconds: downWallSeconds, up_end_to_end_seconds: upWallSeconds,
                                   down_end_to_end_MBps: downWallSeconds > 0
                                       ? Double(downBytes)/downWallSeconds/1_000_000 : 0,
                                   up_end_to_end_MBps: upWallSeconds > 0
                                       ? Double(upBytes)/upWallSeconds/1_000_000 : 0), as: "benchmark.json")
        transition("benchmark_complete",
                   "transfers=\(sequence); seconds=\(elapsed); downBytes=\(downBytes); upBytes=\(upBytes); targetBytes=\(plan.totalBytes)")
        stopTransport()
    }

    func cancel() {
        guard state != "idle" && state != "ack_received" && state != "benchmark_complete" && state != "failed" else { return }
        transition("cancelled")
        stopTransport()
    }

    private func armDeadline(seconds: Int = 5) {
        deadline?.cancel()
        deadline = Task { [weak self] in
            do { try await Task.sleep(for: .seconds(seconds)) } catch { return }
            self?.fail("Operation did not complete within \(seconds) seconds")
        }
    }

    private func fail(_ reason: String) {
        error = reason
        transition("failed", reason)
        recorder.result(state: state, reason: reason)
        stopTransport()
    }

    private func transition(_ state: String, _ detail: String = "") {
        self.state = state
        recorder.event("native_data_\(state)", state: state, detail: detail, directionID: "M5-NAN-DATA")
        if state == "browsing" {
            recorder.connectionMarker("connection_started", state: state)
        } else if ["ack_received", "benchmark_complete", "failed", "cancelled"].contains(state) {
            recorder.flush()
            recorder.connectionMarker("connection_observed", state: state, error: error)
        }
    }

    private func stopTransport() {
        deadline?.cancel(); setup?.cancel()
        browser?.cancel(); browser = nil
        connection?.cancel(); connection = nil
        recorder.flush()
    }
}
