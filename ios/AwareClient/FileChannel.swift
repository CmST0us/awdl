import Foundation
import Network
import os
import WiFiAware

enum TransferError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        if case .message(let text) = self { return text }
        return nil
    }
}

struct RemoteFile: Identifiable, Decodable, Sendable {
    let name: String
    let size: Int64
    var id: String { name }
}

private struct FileList: Decodable {
    let files: [RemoteFile]
}

private struct HTTPReply {
    let status: Int
    let headers: [String: String]
    var length: Int64? { headers["content-length"].flatMap(Int64.init) }
}

final class FileChannel: @unchecked Sendable {
    static let log = Logger(subsystem: Bundle.main.bundleIdentifier!, category: "FileTransfer")
    private let connection: NWConnection
    private let queue = DispatchQueue(label: "aware.file-channel")
    private var buffered = Data()
    // 连接回调及其续体只在 queue 上访问。
    private var opening: CheckedContinuation<Void, Error>?
    private var openTimeout: DispatchWorkItem?
    private var waitReason: String?

    init(endpoint: NWEndpoint, parameters: NWParameters) {
        connection = NWConnection(to: endpoint, using: parameters)
    }

    func cancel() { connection.cancel() }

    @discardableResult
    func open(deviceID: UInt64) async throws -> NWPath {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [self] in
                opening = continuation
                let deadline = DispatchWorkItem { [self] in
                    let reason = waitReason.map { "系统等待原因 \($0)" } ?? "系统未报告等待原因，请检查 Publisher 和手机状态"
                    finishOpen(TransferError.message("连接超时（\(connection.endpoint)）：\(reason)"))
                    connection.cancel()
                }
                openTimeout = deadline
                connection.stateUpdateHandler = { [self] state in
                    switch state {
                    case .ready: finishOpen(nil)
                    case .waiting(let error):
                        // 等待期间路径仍可能恢复，只记录原因，由连接期限决定是否放弃。
                        let reason = error.wifiAware.map { "Wi-Fi Aware \(String(describing: $0))" } ?? error.debugDescription
                        let endpoint = String(describing: connection.endpoint)
                        waitReason = reason
                        Self.log.notice("连接等待 endpoint=\(endpoint, privacy: .public) reason=\(reason, privacy: .public)")
                    case .failed(let error): finishOpen(error)
                    case .cancelled: finishOpen(CancellationError())
                    default: break
                    }
                }
                queue.asyncAfter(deadline: .now() + ClientConfiguration.connectionTimeout, execute: deadline)
                connection.start(queue: queue)
            }
        }
        guard let path = connection.currentPath else {
            cancel()
            throw TransferError.message("连接就绪后没有当前路径（\(connection.endpoint)）")
        }
        guard let aware = try await path.wifiAware else {
            cancel()
            throw TransferError.message("连接路径不是 Wi-Fi Aware（\(connection.endpoint)）")
        }
        guard aware.endpoint.device.id == deviceID else {
            cancel()
            throw TransferError.message("连接到设备 ID \(aware.endpoint.device.id)，所选为 ID \(deviceID)（\(connection.endpoint)）")
        }
        return path
    }

    private func finishOpen(_ error: Error?) {
        guard let continuation = opening else { return }
        opening = nil
        openTimeout?.cancel()
        openTimeout = nil
        connection.stateUpdateHandler = nil
        if let error { continuation.resume(throwing: error) }
        else { continuation.resume() }
    }

    private func send(_ data: Data) async throws {
        try Task.checkCancellation()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async { [self] in
                var completed = false
                let deadline = DispatchWorkItem { [self] in
                    guard !completed else { return }
                    completed = true
                    continuation.resume(throwing: TransferError.message("发送数据超过配置的空闲期限"))
                    connection.cancel()
                }
                queue.asyncAfter(deadline: .now() + ClientConfiguration.ioIdleTimeout, execute: deadline)
                connection.send(content: data, completion: .contentProcessed { error in
                    guard !completed else { return }
                    completed = true
                    deadline.cancel()
                    if let error { continuation.resume(throwing: error) }
                    else { continuation.resume() }
                })
            }
        }
    }

    private func receive(minimumLength: Int = 1) async throws -> Data {
        try Task.checkCancellation()
        return try await withCheckedThrowingContinuation { continuation in
            queue.async { [self] in
                var completed = false
                let deadline = DispatchWorkItem { [self] in
                    guard !completed else { return }
                    completed = true
                    continuation.resume(throwing: TransferError.message("接收数据超过配置的空闲期限"))
                    connection.cancel()
                }
                queue.asyncAfter(deadline: .now() + ClientConfiguration.ioIdleTimeout, execute: deadline)
                connection.receive(minimumIncompleteLength: minimumLength, maximumLength: 256 * 1024) { data, _, _, error in
                    guard !completed else { return }
                    completed = true
                    deadline.cancel()
                    if let error { continuation.resume(throwing: error) }
                    else { continuation.resume(returning: data ?? Data()) }
                }
            }
        }
    }

    private func request(_ method: String, path: String, headers: [String: String] = [:]) async throws {
        var text = "\(method) \(path) HTTP/1.1\r\nHost: fileserver\r\nConnection: keep-alive\r\nAccept-Encoding: identity\r\n"
        for (name, value) in headers { text += "\(name): \(value)\r\n" }
        text += "\r\n"
        try await send(Data(text.utf8))
    }

    private func filePath(_ name: String) throws -> String {
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~")
        guard let encoded = name.addingPercentEncoding(withAllowedCharacters: allowed) else {
            throw TransferError.message("文件名无法编码")
        }
        return "/v1/files/" + encoded
    }

    private func reply() async throws -> HTTPReply {
        let delimiter = Data([13, 10, 13, 10])
        while buffered.range(of: delimiter) == nil {
            let next = try await receive()
            guard !next.isEmpty else { throw TransferError.message("HTTP 响应头未完整接收") }
            buffered.append(next)
            if buffered.range(of: delimiter) == nil && buffered.count > 65536 {
                throw TransferError.message("HTTP 响应头过长")
            }
        }
        let boundary = buffered.range(of: delimiter)!
        guard buffered.distance(from: buffered.startIndex, to: boundary.lowerBound) <= 65536,
              let text = String(data: buffered[..<boundary.lowerBound], encoding: .utf8) else {
            throw TransferError.message("HTTP 响应头无效")
        }
        buffered.removeSubrange(..<boundary.upperBound)
        let lines = text.components(separatedBy: "\r\n")
        let statusLine = lines[0].split(separator: " ", maxSplits: 2)
        guard statusLine.count >= 2, statusLine[0] == "HTTP/1.1",
              let status = Int(statusLine[1]), (100...599).contains(status) else {
            throw TransferError.message("HTTP 状态行无效")
        }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let separator = line.firstIndex(of: ":") else {
                throw TransferError.message("HTTP 响应字段无效")
            }
            let name = line[..<separator].lowercased()
            headers[name] = line[line.index(after: separator)...].trimmingCharacters(in: .whitespaces)
        }
        guard headers["transfer-encoding"] == nil else {
            throw TransferError.message("文件服务响应需要 Content-Length")
        }
        let response = HTTPReply(status: status, headers: headers)
        guard status < 200 || (response.length != nil && response.length! >= 0) else {
            throw TransferError.message("HTTP 响应缺少有效 Content-Length")
        }
        return response
    }

    private func body(_ response: HTTPReply) async throws -> Data {
        guard var remaining = response.length else { throw TransferError.message("HTTP 响应长度缺失") }
        var data = Data()
        while remaining > 0 {
            let chunk: Data
            if !buffered.isEmpty {
                chunk = buffered
                buffered.removeAll(keepingCapacity: false)
            } else { chunk = try await receive() }
            guard !chunk.isEmpty, Int64(chunk.count) <= remaining else {
                throw TransferError.message("HTTP 响应正文长度不匹配")
            }
            data.append(chunk)
            remaining -= Int64(chunk.count)
        }
        return data
    }

    private func require(_ response: HTTPReply, status: Int) async throws {
        guard response.status != status else { return }
        let data = response.length == nil ? Data() : try await body(response)
        let message = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "请求失败"
        throw TransferError.message("HTTP \(response.status)：\(message)")
    }

    func list() async throws -> [RemoteFile] {
        try await request("GET", path: "/v1/files")
        let response = try await reply()
        try await require(response, status: 200)
        return try JSONDecoder().decode(FileList.self, from: await body(response)).files
    }

    func download(_ file: RemoteFile, to destination: URL, offset: Int64, length: Int64, progress: TransferByteCounter,
                  confirm: (String) async throws -> Void = { _ in }) async throws {
        let start = ContinuousClock.now
        let headers = length == 0 ? [:] : ["Range": "bytes=\(offset)-\(offset + length - 1)"]
        try await request("GET", path: filePath(file.name), headers: headers)
        let response = try await reply()
        try await require(response, status: length == 0 ? 200 : 206)
        try await confirm(response.headers["last-modified"] ?? "")
        guard response.length == length,
              length == 0 || response.headers["content-range"] == "bytes \(offset)-\(offset + length - 1)/\(file.size)" else {
            throw TransferError.message("下载响应范围或长度不匹配")
        }
        let headerReceived = ContinuousClock.now
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }
        try output.seek(toOffset: UInt64(offset))
        var remaining = length
        var chunks = 0
        var writeTime = Duration.zero
        var receiveTime = Duration.zero
        var longestReceive = Duration.zero
        var firstPayload: ContinuousClock.Instant?
        var lastPayload = headerReceived
        while remaining > 0 {
            let chunk: Data
            if !buffered.isEmpty {
                chunk = buffered
                buffered.removeAll(keepingCapacity: false)
            } else {
                let receiveStart = ContinuousClock.now
                chunk = try await receive(minimumLength: Int(min(256 * 1024, remaining)))
                let waited = receiveStart.duration(to: .now)
                receiveTime += waited
                longestReceive = max(longestReceive, waited)
            }
            guard !chunk.isEmpty, chunk.count <= remaining else {
                throw TransferError.message("下载字节数不匹配")
            }
            lastPayload = .now
            if firstPayload == nil { firstPayload = lastPayload }
            let writeStart = ContinuousClock.now
            try output.write(contentsOf: chunk)
            writeTime += writeStart.duration(to: .now)
            chunks += 1
            remaining -= Int64(chunk.count)
            progress.add(chunk.count)
        }
        let elapsed = start.duration(to: .now)
        let seconds = Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
        let writeSeconds = Double(writeTime.components.seconds) + Double(writeTime.components.attoseconds) / 1e18
        Self.log.info("下载分片完成 bytes=\(length) chunks=\(chunks) seconds=\(seconds, format: .fixed(precision: 3)) write_seconds=\(writeSeconds, format: .fixed(precision: 3))")
        func stageSeconds(_ duration: Duration) -> Double {
            Double(duration.components.seconds) + Double(duration.components.attoseconds) / 1e18
        }
        Self.log.info("下载分片阶段 offset=\(offset) header_seconds=\(stageSeconds(start.duration(to: headerReceived)), format: .fixed(precision: 3)) first_seconds=\(stageSeconds(start.duration(to: firstPayload ?? headerReceived)), format: .fixed(precision: 3)) last_seconds=\(stageSeconds(start.duration(to: lastPayload)), format: .fixed(precision: 3)) receive_seconds=\(stageSeconds(receiveTime), format: .fixed(precision: 3)) longest_receive_seconds=\(stageSeconds(longestReceive), format: .fixed(precision: 3))")
    }

    func upload(_ url: URL, progress: TransferByteCounter) async throws -> Int64 {
        let input = try FileHandle(forReadingFrom: url)
        defer { try? input.close() }
        let size = try input.seekToEnd()
        progress.setTotal(Int64(size))
        try input.seek(toOffset: 0)
        var headers = ["Content-Length": String(size), "Content-Type": "application/octet-stream"]
        if size > 0 { headers["Expect"] = "100-continue" }
        try await request("PUT", path: filePath(url.lastPathComponent), headers: headers)
        if size > 0 { try await require(reply(), status: 100) }
        var sent: UInt64 = 0
        while sent < size {
            guard let data = try input.read(upToCount: min(256 * 1024, Int(size - sent))), !data.isEmpty else {
                throw TransferError.message("上传源文件在传输中被截断")
            }
            try await send(data)
            sent += UInt64(data.count)
            progress.add(data.count)
        }
        let response = try await reply()
        if response.status != 200 { try await require(response, status: 201) }
        let receipt = try JSONDecoder().decode(RemoteFile.self, from: await body(response))
        guard receipt.size == Int64(size), receipt.name == url.lastPathComponent else {
            throw TransferError.message("上传回执文件名或长度不匹配")
        }
        return Int64(size)
    }
}
