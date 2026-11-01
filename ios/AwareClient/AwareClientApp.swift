import SwiftUI
import UniformTypeIdentifiers

@main
struct AwareClientApp: App {
    var body: some Scene { WindowGroup { ContentView() } }
}

struct ContentView: View {
    @State private var model = AwareModel()
    @State private var importing = false

    var body: some View {
        NavigationStack {
            List {
                Section("设备") {
                    Picker("已配对设备", selection: $model.selectedID) {
                        Text("请选择").tag(UInt64(0))
                        ForEach(model.devices, id: \.id) { device in
                            Text("\(device.pairingInfo?.pairingName ?? "Wi-Fi Aware") · ID \(device.id)").tag(device.id)
                        }
                    }
                    .disabled(model.busy || model.connected)
                    HStack {
                        Button(model.connected ? "断开" : "连接") {
                            if model.connected { model.disconnect() } else { model.connect() }
                        }
                        .disabled(model.busy || model.selectedID == 0)
                        Spacer()
                        Button("配对新设备") { model.pair() }.disabled(model.busy)
                    }
                    .buttonStyle(.borderless)
                    Text(model.status).foregroundStyle(.secondary)
                }
                Section("传输") {
                    Picker("下载连接数", selection: $model.streams) {
                        Text("单连接").tag(1)
                        Text("4 路自适应").tag(4)
                    }.disabled(model.busy)
                    Button("上传文件") { importing = true }.disabled(!model.connected || model.busy)
                    if model.busy {
                        HStack {
                            ProgressView()
                            Spacer()
                            Button("取消", role: .cancel) { model.cancel() }
                        }
                    }
                    if !model.rate.isEmpty { Text(model.rate).monospacedDigit() }
                    if let url = model.exportedFile { ShareLink("打开下载文件", item: url) }
                }
                if let progress = model.transfer {
                    Section("传输速度") { TransferSpeedView(progress: progress) }
                }
                Section {
                    ForEach(model.files) { file in
                        Button { model.download(file) } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(file.name)
                                    Text(ByteCountFormatter.string(fromByteCount: file.size, countStyle: .file))
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: "arrow.down.circle")
                            }
                        }.disabled(model.busy)
                    }
                } header: {
                    HStack {
                        Text("共享文件")
                        Spacer()
                        Button("刷新") { model.loadFiles() }.disabled(!model.connected || model.busy)
                    }
                }
                if let error = model.error {
                    Section("提示") { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                }
            }
            .navigationTitle("Wi-Fi Aware")
            .toolbar { Button("刷新设备", systemImage: "arrow.clockwise") { model.refresh() }.disabled(model.busy) }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
                switch result {
                case .success(let url): model.upload(url)
                case .failure(let error): model.error = error.localizedDescription
                }
            }
        }
    }
}
