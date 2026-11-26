import SwiftUI

struct CameraSettingsView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section(tr("Демо-режим для перевірки без ПК", "Offline Demo Mode (No PC Required)")) {
                    Button {
                        engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                        dismiss()
                    } label: {
                        Label(tr("Open Demo Cine Studio", "Open Demo Cine Studio"), systemImage: "sparkles.tv.fill")
                            .font(.headline)
                            .foregroundColor(CineTheme.lavenderBright)
                    }

                    if engine.isDemoMode {
                        Button(role: .destructive) {
                            engine.exitDemoMode()
                            dismiss()
                        } label: {
                            Label(tr("Вийти з Demo Cine Studio", "Exit Demo Cine Studio"), systemImage: "xmark.circle")
                        }
                    }

                    Text(tr("Запускає інтерактивний видошукач, ручні коліщатка Shutter/ISO/WB/Focus A↔B, перемикання Tally (STBY → PVW → PGM), iPhone Duo та Duplex Intercom без підключеного комп'ютера.",
                            "Launches an interactive viewfinder with manual Shutter/ISO/WB/Focus A↔B wheels, live Tally transitions (STBY → PVW → PGM), iPhone Duo and Duplex Intercom without a desktop studio."))
                        .font(.caption)
                        .foregroundColor(CineTheme.textMuted)
                }

                Section(tr("Студія LanVid Video / Studio", "LanVid Video / Studio Connection")) {
                    HStack {
                        Text(tr("Назва камери", "Camera Label"))
                        Spacer()
                        TextField("CAM A", text: $engine.cameraLabel)
                            .multilineTextAlignment(.trailing)
                    }
                    HStack {
                        Text(tr("IP-адреса", "Studio IP"))
                        Spacer()
                        TextField("192.168.1.24", text: $engine.destIp)
                            .multilineTextAlignment(.trailing)
                            .keyboardType(.decimalPad)
                    }
                    Stepper(tr("Порт відео: ", "Video Port: ") + "\(engine.videoPort)", value: $engine.videoPort, in: 1024...65535)
                    Stepper(tr("Порт Tally/Інтерком: ", "Tally/Intercom Port: ") + "\(engine.tallyPort)", value: $engine.tallyPort, in: 1024...65535)
                }

                Section(tr("Відео та індикація", "Video & Assistants")) {
                    Picker(tr("Роздільна здатність", "Resolution"), selection: $engine.resolution) {
                        ForEach(CineResolution.allCases) { r in Text(r.rawValue).tag(r) }
                    }
                    Picker(tr("Кодек", "Codec"), selection: $engine.codec) {
                        ForEach(CineCodec.allCases) { c in Text(c.rawValue).tag(c) }
                    }
                    Toggle(tr("Гістограма та VU-метри", "Histogram & VU Meters"), isOn: $engine.showHistogram)
                    Toggle(tr("Сітка кадрування", "Framing Guides"), isOn: $engine.showGuides)
                    Toggle(tr("Фокус-пікінг", "Focus Peaking"), isOn: $engine.focusPeaking)
                    Toggle(tr("Зебра пересвітів", "Exposure Zebra"), isOn: $engine.zebra)
                }

                Section(tr("Конфіденційність та Підтримка", "Privacy & Support")) {
                    Link(destination: AppLinks.privacyPolicy) {
                        Label(tr("Політика конфіденційності", "Privacy Policy"), systemImage: "hand.raised.fill")
                    }
                    Link(destination: AppLinks.support) {
                        Label(tr("Підтримка та інструкція", "Support & Help"), systemImage: "questionmark.circle.fill")
                    }
                    HStack {
                        Text(tr("Версія", "Version"))
                        Spacer()
                        Text("2.7.0 • Local Wi-Fi Only")
                            .font(.caption.monospaced())
                            .foregroundColor(CineTheme.textMuted)
                    }
                }
            }
            .navigationTitle(tr("Налаштування", "Settings"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(tr("Готово", "Done")) { dismiss() }
                }
            }
        }
        .preferredColorScheme(.dark)
    }
}
