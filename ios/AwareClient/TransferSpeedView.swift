import Charts
import SwiftUI

struct TransferSpeedView: View {
    let progress: TransferProgress

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("\(progress.direction) · \(progress.filename)")
                .font(.subheadline).lineLimit(2)
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .firstTextBaseline, spacing: 24) { speedLabels }
                VStack(alignment: .leading, spacing: 8) { speedLabels }
            }
            ProgressView(value: Double(progress.completedBytes), total: Double(max(progress.totalBytes, 1)))
                .accessibilityLabel("传输进度")
            HStack {
                Text("\(size(progress.completedBytes)) / \(size(progress.totalBytes))")
                Spacer()
                Text("\(progress.elapsed, specifier: "%.1f") 秒")
            }
            .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            Chart(progress.samples) { sample in
                AreaMark(x: .value("时间（秒）", sample.seconds),
                         y: .value("速度（MB/s）", sample.megabytesPerSecond))
                    .foregroundStyle(Color.accentColor.opacity(0.12))
                LineMark(x: .value("时间（秒）", sample.seconds),
                         y: .value("速度（MB/s）", sample.megabytesPerSecond))
                    .foregroundStyle(Color.accentColor)
            }
            .chartXScale(domain: max(0, progress.elapsed - 60)...max(10, progress.elapsed))
            .chartYScale(domain: 0...max(1, (progress.samples.map(\.megabytesPerSecond).max() ?? 0) * 1.1))
            .chartXAxisLabel("时间（秒）")
            .chartYAxisLabel("MB/s")
            .frame(height: 150)
            .accessibilityLabel("最近 60 秒\(progress.direction)速度曲线")
            Text(progress.isRunning ? "最近 60 秒 · 每 0.5 秒更新" : "最近 60 秒 · 传输已结束")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder private var speedLabels: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("实时速度").font(.caption).foregroundStyle(.secondary)
            Text("\(progress.currentSpeed, specifier: "%.2f") MB/s")
                .font(.title2.weight(.semibold)).monospacedDigit()
        }
        VStack(alignment: .leading, spacing: 2) {
            Text("平均速度").font(.caption).foregroundStyle(.secondary)
            Text("\(progress.averageSpeed, specifier: "%.2f") MB/s")
                .font(.headline).monospacedDigit()
        }
    }

    private func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}
