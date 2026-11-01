import Foundation
import Observation
import os

final class TransferByteCounter: Sendable {
    private struct State {
        var completed: Int64 = 0
        var total: Int64 = 0
    }
    private let state = OSAllocatedUnfairLock(initialState: State())

    func setTotal(_ bytes: Int64) { state.withLock { $0.total = bytes } }
    func add(_ bytes: Int) { state.withLock { $0.completed += Int64(bytes) } }
    func snapshot() -> (completed: Int64, total: Int64) {
        state.withLock { ($0.completed, $0.total) }
    }
}

struct SpeedSample: Identifiable {
    let seconds: Double
    let megabytesPerSecond: Double
    var id: Double { seconds }
}

@MainActor @Observable
final class TransferProgress {
    let direction: String
    let filename: String
    let counter = TransferByteCounter()
    private(set) var completedBytes: Int64 = 0
    private(set) var totalBytes: Int64 = 0
    private(set) var elapsed: Double = 0
    private(set) var currentSpeed: Double = 0
    private(set) var samples = [SpeedSample(seconds: 0, megabytesPerSecond: 0)]
    private(set) var isRunning = true
    private let start: ContinuousClock.Instant
    private var lastSample: ContinuousClock.Instant
    private var lastInterval: (start: ContinuousClock.Instant, bytes: Int64)?
    private var sampling: Task<Void, Never>?

    var averageSpeed: Double { Double(completedBytes) / max(elapsed, 0.001) / 1e6 }

    init(direction: String, filename: String, totalBytes: Int64 = 0,
         start: ContinuousClock.Instant) {
        self.direction = direction
        self.filename = filename
        self.totalBytes = totalBytes
        self.start = start
        lastSample = start
        counter.setTotal(totalBytes)
        sampling = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .milliseconds(500)) }
                catch { return }
                // 定时恰在 finish() 之前到期时，已排队的续体不再追加样本。
                guard let self, !Task.isCancelled else { return }
                self.sample()
            }
        }
    }

    private func sample() {
        let now = ContinuousClock.now
        let snapshot = counter.snapshot()
        let interval = lastSample.duration(to: now).seconds
        elapsed = start.duration(to: now).seconds
        currentSpeed = Double(snapshot.completed - completedBytes) / max(interval, 0.001) / 1e6
        lastInterval = (lastSample, completedBytes)
        completedBytes = snapshot.completed
        totalBytes = snapshot.total
        lastSample = now
        samples.append(SpeedSample(seconds: elapsed, megabytesPerSecond: currentSpeed))
        samples.removeAll { $0.seconds < elapsed - 60 }
    }

    func finish() {
        guard isRunning else { return }
        sampling?.cancel()
        sampling = nil
        // 不足半个采样周期的尾段并入前一区间，避免最后一块数据除以极短区间形成尖峰。
        if let lastInterval, lastSample.duration(to: .now) < .milliseconds(250) {
            samples.removeLast()
            lastSample = lastInterval.start
            completedBytes = lastInterval.bytes
        }
        sample()
        isRunning = false
        currentSpeed = 0
    }
}

private extension Duration {
    var seconds: Double { Double(components.seconds) + Double(components.attoseconds) / 1e18 }
}
