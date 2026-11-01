import AccessorySetupKit
import Foundation
import Network
import Observation
import UIKit
import WiFiAware

private struct DownloadRange: Sendable {
    let offset: Int64
    let length: Int64
}

private actor DownloadRangeQueue {
    private var pending: [DownloadRange]
    private var claimed: Set<ObjectIdentifier> = []
    private var modified: String?

    init(fileSize: Int64, chunkSize: Int64) {
        var ranges: [DownloadRange] = []
        var offset: Int64 = 0
        while offset < fileSize {
            let length = min(chunkSize, fileSize - offset)
            ranges.append(DownloadRange(offset: offset, length: length))
            offset += length
        }
        pending = ranges.reversed()
    }

    func next(for client: FileChannel) -> DownloadRange? {
        guard let range = pending.popLast() else { return nil }
        claimed.insert(ObjectIdentifier(client))
        return range
    }

    func unclaimed(_ clients: [FileChannel]) -> [FileChannel] {
        clients.filter { !claimed.contains(ObjectIdentifier($0)) }
    }

    var claimedCount: Int { claimed.count }

    // 文件服务按名称打开每个范围请求，分片的 Last-Modified 与首个分片不同说明同名文件已被替换。
    func confirm(modified value: String) throws {
        if modified == nil { modified = value }
        guard modified == value else { throw TransferError.message("远端文件在下载期间已更改") }
    }
}

@MainActor @Observable
final class AwareModel {
    static let serviceName = ClientConfiguration.serviceName
    var devices: [WAPairedDevice] = []
    var selectedID: UInt64 = 0 {
        didSet {
            // 取得端点前改选设备，结束对原设备的发现，需重新连接。
            guard selectedID != oldValue, browser != nil, endpoint == nil else { return }
            disconnect()
            status = "选择设备后连接"
        }
    }
    var files: [RemoteFile] = []
    var streams = 1
    var status = "正在读取配对设备"
    var error: String?
    var rate = ""
    var transfer: TransferProgress?
    var busy = false
    var connected = false
    var exportedFile: URL?
    private var activated = false
    private let session = ASAccessorySession()
    private var browser: NWBrowser?
    private var endpoint: NWEndpoint?
    private var parameters: NWParameters?
    private var channels: [FileChannel] = []
    private var operation: Task<Void, Never>?

    init() {
        do {
            let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            for partial in try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
                where partial.lastPathComponent.hasPrefix(".download-") {
                try FileManager.default.removeItem(at: partial)
            }
        } catch { self.error = "清理未完成下载失败：\(error.localizedDescription)" }
        session.activate(on: .main) { [weak self] event in
            MainActor.assumeIsolated {
                guard let self else { return }
                switch event.eventType {
                case .activated:
                    self.activated = true
                    self.refresh()
                case .accessoryAdded, .accessoryChanged, .accessoryRemoved, .pickerDidDismiss:
                    self.refresh()
                case .invalidated:
                    self.activated = false
                    self.error = event.error?.localizedDescription ?? "系统配对会话已失效，请重新打开 App"
                default: break
                }
            }
        }
    }

    func refresh() {
        guard activated else { return }
        Task {
            do {
                let authorized = Set(session.accessories.filter { $0.state == .authorized }
                    .map(\.wifiAwarePairedDeviceID))
                let all = try await WAPairedDevice.allDevices.current() ?? [:]
                devices = all.values.filter { authorized.contains($0.id) }.sorted { $0.id < $1.id }
                if !devices.contains(where: { $0.id == selectedID }) {
                    disconnect()
                    selectedID = devices.count == 1 ? devices[0].id : 0
                }
                if !busy && !connected { status = devices.isEmpty ? "请先与 Wi-Fi Aware 设备 配对" : "选择设备后连接" }
            } catch { self.error = error.localizedDescription }
        }
    }

    func pair() {
        guard activated else { return }
        let descriptor = ASDiscoveryDescriptor()
        descriptor.wifiAwareServiceName = Self.serviceName
        descriptor.wifiAwareServiceRole = .subscriber
        let item = ASPickerDisplayItem(name: "Wi-Fi Aware 设备", productImage: UIImage(systemName: "wifi.router")!, descriptor: descriptor)
        Task {
            do { try await session.showPicker(for: [item]); refresh() }
            catch { self.error = error.localizedDescription }
        }
    }

    func connect() {
        disconnect()
        error = nil
        guard let device = devices.first(where: { $0.id == selectedID }),
              let service = WASubscribableService.allServices[Self.serviceName] else {
            error = "请选择已授权的设备"
            return
        }
        let provider = WASubscriberBrowser.wifiAware(.connecting(to: .selected([device]), from: service))
        let parameters = provider.configureParameters(.tcp)
        parameters.wifiAware = WAParameters(performanceMode: .bulk)
        self.parameters = parameters
        let browser = NWBrowser(for: provider.makeDescriptor(), using: parameters)
        self.browser = browser
        status = "正在发现 Wi-Fi Aware 设备"
        browser.stateUpdateHandler = { [weak self, weak browser] state in
            MainActor.assumeIsolated {
                guard let self, self.browser === browser else { return }
                if case .failed(let error) = state {
                    browser?.cancel()
                    self.browser = nil
                    // 服务发现的结束不撤销已经获得的端点或正在收发的连接。
                    if self.endpoint == nil {
                        self.status = "未找到设备"
                        self.error = error.localizedDescription
                    }
                }
            }
        }
        browser.browseResultsChangedHandler = { [weak self, weak browser] results, _ in
            MainActor.assumeIsolated {
                guard let self, self.browser === browser, self.endpoint == nil else { return }
                do {
                    for result in results {
                        guard let aware = try provider.makeEndpoint(from: result), aware.device.id == device.id else { continue }
                        self.endpoint = result.endpoint
                        self.connected = true
                        self.loadFiles()
                        break
                    }
                } catch { self.error = error.localizedDescription }
            }
        }
        browser.start(queue: .main)
    }

    func disconnect() {
        cancel()
        browser?.cancel()
        browser = nil
        endpoint = nil
        parameters = nil
        connected = false
        files = []
        status = "已断开"
    }

    func cancel() {
        operation?.cancel()
        channels.forEach { $0.cancel() }
    }

    private func perform(_ message: String, body: @escaping () async throws -> Void) {
        guard !busy else { return }
        busy = true
        UIApplication.shared.isIdleTimerDisabled = true
        status = message
        error = nil
        rate = ""
        operation = Task {
            defer {
                channels.forEach { $0.cancel() }
                channels = []
                busy = false
                UIApplication.shared.isIdleTimerDisabled = false
                operation = nil
            }
            do { try await body() }
            catch {
                if error is CancellationError || Task.isCancelled {
                    status = "已取消"
                } else {
                    self.error = error.localizedDescription
                    status = "操作失败"
                }
            }
        }
    }

    private func channel() throws -> FileChannel {
        guard let endpoint, let parameters else { throw TransferError.message("尚未连接 Wi-Fi Aware 设备") }
        let channel = FileChannel(endpoint: endpoint, parameters: parameters)
        channels.append(channel)
        return channel
    }

    func loadFiles() {
        perform("正在读取文件列表") { [self] in
            let channel = try channel()
            try await channel.open(deviceID: selectedID)
            files = try await channel.list()
            status = "已连接 · ID \(selectedID) · Bulk"
        }
    }

    func download(_ file: RemoteFile) {
        perform("正在下载 \(file.name)") { [self] in
            let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            let destination = folder.appendingPathComponent(file.name)
            guard URL(fileURLWithPath: file.name).lastPathComponent == file.name, !file.name.hasPrefix(".") else {
                throw TransferError.message("远端文件名无效")
            }
            let capacity = try folder.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
                .volumeAvailableCapacityForImportantUsage
            if let capacity, capacity < file.size + 16 * 1024 * 1024 {
                throw TransferError.message("手机可用空间不足")
            }
            let temporary = folder.appendingPathComponent(".download-\(UUID().uuidString)")
            FileManager.default.createFile(atPath: temporary.path, contents: nil)
            defer { try? FileManager.default.removeItem(at: temporary) }
            let output = try FileHandle(forWritingTo: temporary)
            try output.truncate(atOffset: UInt64(file.size))
            try output.close()
            let chunkSize = max(1024 * 1024, min(4 * 1024 * 1024, file.size / Int64(streams * 4)))
            let count = file.size == 0 ? 1 : min(streams, Int((file.size + chunkSize - 1) / chunkSize))
            let first = try channel()
            let id = selectedID
            let start = ContinuousClock.now
            let progress = TransferProgress(direction: "下载", filename: file.name, totalBytes: file.size, start: start)
            transfer = progress
            defer { progress.finish() }
            let counter = progress.counter
            let path = try await first.open(deviceID: id)
            try Task.checkCancellation()
            let extras: [FileChannel]
            if count > 1 {
                guard let endpoint = path.remoteEndpoint,
                      let interface = path.availableInterfaces.first(where: { $0.name.hasPrefix("nan") }),
                      let parameters else {
                    throw TransferError.message("未取得已连接设备的 NAN 地址和接口")
                }
                extras = (1..<count).map { _ in
                    let extra = parameters.copy()
                    extra.requiredInterface = interface
                    return FileChannel(endpoint: endpoint, parameters: extra)
                }
                channels += extras
            } else {
                extras = []
            }
            let queue = count == 1 ? nil : DownloadRangeQueue(fileSize: file.size, chunkSize: chunkSize)
            try await withThrowingTaskGroup(of: Void.self) { group in
                if let queue {
                    group.addTask {
                        while let range = await queue.next(for: first) {
                            try Task.checkCancellation()
                            try await first.download(file, to: temporary, offset: range.offset, length: range.length,
                                                     progress: counter) { try await queue.confirm(modified: $0) }
                        }
                        // 分片已全部领取，尚未领到分片的附加连接不再需要。
                        for client in await queue.unclaimed(extras) { client.cancel() }
                    }
                    for client in extras {
                        group.addTask {
                            do { try await client.open(deviceID: id) }
                            catch {
                                // 附加连接只增加并行度，未能加入时由其他连接完成剩余分片。
                                client.cancel()
                                if !(error is CancellationError) {
                                    FileChannel.log.error("附加连接未加入下载：\(error.localizedDescription, privacy: .public)")
                                }
                                return
                            }
                            while let range = await queue.next(for: client) {
                                try Task.checkCancellation()
                                try await client.download(file, to: temporary, offset: range.offset, length: range.length,
                                                          progress: counter) { try await queue.confirm(modified: $0) }
                            }
                        }
                    }
                } else {
                    group.addTask {
                        try await first.download(file, to: temporary, offset: 0, length: file.size, progress: counter)
                    }
                }
                do { for try await _ in group {} }
                catch { first.cancel(); extras.forEach { $0.cancel() }; group.cancelAll(); throw error }
            }
            let connections = await queue?.claimedCount ?? 1
            try Task.checkCancellation()
            if FileManager.default.fileExists(atPath: destination.path) {
                _ = try FileManager.default.replaceItemAt(destination, withItemAt: temporary)
            } else {
                try FileManager.default.moveItem(at: temporary, to: destination)
            }
            exportedFile = destination
            showRate(bytes: file.size, elapsed: start.duration(to: .now), direction: "下载", connections: connections)
        }
    }

    func upload(_ url: URL) {
        perform("正在上传 \(url.lastPathComponent)") { [self] in
            let access = url.startAccessingSecurityScopedResource()
            defer { if access { url.stopAccessingSecurityScopedResource() } }
            let client = try channel()
            let start = ContinuousClock.now
            let progress = TransferProgress(direction: "上传", filename: url.lastPathComponent, start: start)
            transfer = progress
            defer { progress.finish() }
            try await client.open(deviceID: selectedID)
            let bytes = try await client.upload(url, progress: progress.counter)
            showRate(bytes: bytes, elapsed: start.duration(to: .now), direction: "上传")
        }
    }

    private func showRate(bytes: Int64, elapsed: Duration, direction: String, connections: Int = 1) {
        let seconds = Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
        rate = String(format: "%@ %.2f MB/s · %.2f 秒 · %@ · %ld 连接", direction,
                      Double(bytes) / max(seconds, 0.001) / 1e6, seconds,
                      ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file), connections)
        status = "\(direction)完成"
    }
}
