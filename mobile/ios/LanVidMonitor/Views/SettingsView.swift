import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var engine: MonitorEngine

    private var versionText: String {
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let b = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"
        return "\(v) (\(b))"
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                Card(title: tr("Цей телефон", "This phone")) {
                    TextField(tr("Назва у списку LanVid Studio", "Name shown in LanVid Studio"), text: $engine.clientName)
                        .textFieldStyle(.roundedBorder)
                    Text(tr("До 15 символів — так телефон підписаний у списку смартфонів на ПК.",
                            "Up to 15 characters — how this phone is labelled in LanVid Studio's phone list."))
                        .font(.caption).foregroundStyle(Theme.muted)
                }

                Card(title: tr("Моніторинговий стереопотік", "Monitor stereo stream")) {
                    Text(tr("Якість", "Quality")).font(.caption).foregroundStyle(Theme.muted)
                    SegmentRow(items: StreamQuality.allCases, selection: $engine.quality) { $0.label }
                    Text(tr("Буфер", "Buffer")).font(.caption).foregroundStyle(Theme.muted)
                    SegmentRow(items: MonitorBuffer.allCases, selection: $engine.buffer) { "\($0.rawValue) " + tr("мс", "ms") }
                    Text(tr("Менший буфер — менша затримка в навушниках, але потрібна стабільна Wi-Fi мережа.",
                            "A smaller buffer means lower latency in your headphones, but needs a stable Wi-Fi network."))
                        .font(.caption2).foregroundStyle(Theme.muted)
                }

                Card(title: tr("Демо-режим", "Demo Studio Preview")) {
                    Button {
                        if engine.isDemo { engine.disconnect() } else { engine.startDemo() }
                    } label: {
                        Label(engine.isDemo ? tr("Вийти з Demo Studio", "Leave Demo Studio")
                                            : tr("Відкрити Demo Studio", "Open Demo Studio"),
                              systemImage: engine.isDemo ? "stop.circle" : "play.circle.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered).tint(Theme.accent2)
                    Text(tr("Симульована студія без мережі та звуку — щоб ознайомитися з усіма екранами.",
                            "A simulated studio with no network and no audio — to explore every screen."))
                        .font(.caption).foregroundStyle(Theme.muted)
                }

                Card(title: tr("Конфіденційність і підтримка", "Privacy & Support")) {
                    Text(tr("LanVid Monitor працює лише у вашій локальній Wi-Fi мережі. Застосунок не збирає персональних даних і не надсилає звук чи телеметрію на зовнішні сервери.",
                            "LanVid Monitor works only on your local Wi-Fi network. It does not collect personal data and sends no audio or telemetry to external servers."))
                        .font(.footnote).foregroundStyle(Theme.muted)
                    Link(destination: AppLinks.privacyPolicy) {
                        Label(tr("Політика конфіденційності", "Privacy Policy"), systemImage: "hand.raised")
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .accessibilityIdentifier("privacyLink")
                    Link(destination: AppLinks.support) {
                        Label(tr("Підтримка", "Support"), systemImage: "questionmark.circle")
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .accessibilityIdentifier("supportLink")
                    Button {
                        if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                    } label: {
                        Label(tr("Дозвіл «Локальна мережа» в Параметрах iOS", "Local Network permission in iOS Settings"),
                              systemImage: "gearshape")
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .tint(Theme.accent2)

                Card(title: tr("Про застосунок", "About")) {
                    HStack {
                        Text("LanVid Monitor").foregroundStyle(Theme.text)
                        Spacer()
                        Text(versionText).foregroundStyle(Theme.muted).font(.footnote.monospacedDigit())
                    }
                    Text(tr("Працює з LanVid Studio для Windows, macOS і Linux.", "Works with LanVid Studio for Windows, macOS and Linux."))
                        .font(.caption).foregroundStyle(Theme.muted)
                }
            }
            .padding()
        }
        .background(Theme.bg.ignoresSafeArea())
    }
}
