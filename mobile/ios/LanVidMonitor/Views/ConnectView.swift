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
                    Card(title: tr("Підключено", "Connected")) {
                        HStack {
                            Circle().fill(engine.telemetry.receiving ? Theme.green : Theme.amber).frame(width: 10, height: 10)
                            VStack(alignment: .leading) {
                                Text(s.name).font(.headline).foregroundStyle(Theme.text)
                                Text(engine.isDemo ? engine.audioStatus : "\(s.host):\(s.port) · \(engine.audioStatus)")
                                    .font(.caption).foregroundStyle(Theme.muted)
                            }
                            Spacer()
                            Button(tr("Відключити", "Disconnect")) { engine.disconnect() }
                                .buttonStyle(.borderedProminent).tint(Theme.red)
                        }
                        Text(engine.isDemo
                             ? tr("Симульована студія з 8 каналами: усі екрани працюють без ПК.",
                                  "Simulated 8-channel studio: every screen works without a PC.")
                             : (engine.telemetry.receiving
                                ? tr("Звук надходить · буфер ", "Receiving audio · buffer ")
                                    + String(format: "%.1f ", engine.telemetry.bufferMs) + tr("мс", "ms")
                                : tr("Очікування звуку від LanVid Studio…", "Waiting for audio from LanVid Studio…")))
                            .font(.footnote).foregroundStyle(Theme.muted)
                    }
                }

                Card(title: tr("LanVid Studio у мережі", "LanVid Studio on your network"),
                     trailing: AnyView(Button { browser.start() } label: { Image(systemName: "arrow.clockwise") }
                        .tint(Theme.accent2))) {
                    if browser.studios.isEmpty {
                        HStack(spacing: 10) {
                            ProgressView().tint(Theme.accent2)
                            Text(browser.status.isEmpty ? tr("Пошук…", "Searching…") : browser.status)
                                .font(.footnote).foregroundStyle(Theme.muted)
                        }
                        Text(tr("У LanVid Studio на ПК увімкніть «Мобільний моніторинг» — студія з'явиться тут автоматично.",
                                "In LanVid Studio on your PC turn on “Mobile Monitoring” — the studio appears here automatically."))
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
                                Text(engine.connected?.id == s.id ? tr("Підключено", "Connected") : tr("Підключити", "Connect"))
                                    .font(.footnote.weight(.semibold)).foregroundStyle(Theme.accent2)
                            }
                            .padding(10)
                            .background(RoundedRectangle(cornerRadius: 12).fill(Theme.cardHi))
                        }
                        .buttonStyle(.plain)
                    }
                }

                Card(title: tr("Підключення за IP", "Connect by IP")) {
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
                    } label: { Text(tr("Підключити", "Connect")).frame(maxWidth: .infinity) }
                        .buttonStyle(.borderedProminent).tint(Theme.accent)
                        .disabled(host.isEmpty)
                }

                Card(title: tr("Демо-режим", "Demo Studio Preview")) {
                    Text(tr("Немає студії під рукою? Відкрийте симульовану студію з 8 каналами й перегляньте мікшер та обробку звуку.",
                            "No studio at hand? Open a simulated 8-channel studio and explore the mixer and the sound processing."))
                        .font(.footnote).foregroundStyle(Theme.muted)
                    Button { engine.startDemo() } label: {
                        Label(tr("Відкрити Demo Studio", "Open Demo Studio"), systemImage: "play.circle.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered).tint(Theme.accent2)
                    .accessibilityIdentifier("openDemo")
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
