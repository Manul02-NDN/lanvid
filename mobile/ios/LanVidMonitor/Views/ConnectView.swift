import SwiftUI

struct ConnectView: View {
    @EnvironmentObject var engine: MonitorEngine
    @StateObject private var browser = StudioBrowser()
    @State private var host = ""
    @State private var port = "5020"

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                if let s = engine.connected {
                    Card(title: "Підключено") {
                        HStack {
                            Circle().fill(engine.telemetry.receiving ? Theme.green : Theme.amber).frame(width: 10, height: 10)
                            VStack(alignment: .leading) {
                                Text(s.name).font(.headline).foregroundStyle(Theme.text)
                                Text("\(s.host):\(s.port) · \(engine.audioStatus)").font(.caption).foregroundStyle(Theme.muted)
                            }
                            Spacer()
                            Button("Відключити") { engine.disconnect() }
                                .buttonStyle(.borderedProminent).tint(Theme.red)
                        }
                        Text(engine.telemetry.receiving ? "Звук надходить · буфер \(String(format: "%.1f", engine.telemetry.bufferMs)) мс"
                                                        : "Очікування звуку від LanVid Studio…")
                            .font(.footnote).foregroundStyle(Theme.muted)
                    }
                }

                Card(title: "LanVid Studio у мережі",
                     trailing: AnyView(Button { browser.start() } label: { Image(systemName: "arrow.clockwise") }
                        .tint(Theme.accent2))) {
                    if browser.studios.isEmpty {
                        HStack(spacing: 10) {
                            ProgressView().tint(Theme.accent2)
                            Text(browser.status.isEmpty ? "Пошук…" : browser.status)
                                .font(.footnote).foregroundStyle(Theme.muted)
                        }
                        Text("У LanVid Studio на ПК увімкніть «Мобільний моніторинг» — студія з'явиться тут автоматично.")
                            .font(.caption).foregroundStyle(Theme.muted)
                    }
                    ForEach(browser.studios) { s in
                        Button { engine.connect(to: s) } label: {
                            HStack {
                                Image(systemName: "desktopcomputer").foregroundStyle(Theme.accent2)
                                VStack(alignment: .leading) {
                                    Text(s.name).foregroundStyle(Theme.text)
                                    Text("\(s.host):\(s.port)").font(.caption).foregroundStyle(Theme.muted)
                                }
                                Spacer()
                                Text(engine.connected?.id == s.id ? "Підключено" : "Підключити")
                                    .font(.footnote.weight(.semibold)).foregroundStyle(Theme.accent2)
                            }
                            .padding(10)
                            .background(RoundedRectangle(cornerRadius: 12).fill(Theme.cardHi))
                        }
                        .buttonStyle(.plain)
                    }
                }

                Card(title: "Підключення за IP") {
                    HStack {
                        TextField("192.168.1.20", text: $host)
                            .keyboardType(.numbersAndPunctuation).textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                        TextField("5020", text: $port).keyboardType(.numberPad).frame(width: 70)
                    }
                    .textFieldStyle(.roundedBorder)
                    Button {
                        let p = UInt16(port) ?? 5020
                        engine.connect(to: StudioEndpoint(name: "LanVid Studio (\(host))", host: host, port: p))
                    } label: { Text("Підключити").frame(maxWidth: .infinity) }
                        .buttonStyle(.borderedProminent).tint(Theme.accent)
                        .disabled(host.isEmpty)
                }

                Card(title: "Цей телефон") {
                    TextField("Назва у списку LanVid Studio", text: $engine.clientName)
                        .textFieldStyle(.roundedBorder)
                    Text("До 15 символів — так телефон підписаний у списку смартфонів на ПК.")
                        .font(.caption).foregroundStyle(Theme.muted)
                }

                if let e = engine.lastError {
                    Text(e).font(.footnote).foregroundStyle(Theme.red)
                }
            }
            .padding()
        }
        .background(Theme.bg.ignoresSafeArea())
        .onAppear {
            browser.start()
            if host.isEmpty { host = engine.lastManualHost; port = String(engine.lastManualPort) }
        }
        .onDisappear { browser.stop() }
    }
}
