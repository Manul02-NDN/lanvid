import SwiftUI

struct CameraSettingsView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.studio) private var s
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    // Section 1: Theme & Demo
                    sectionBlock(title: tr("Інтерфейс та демо", "Appearance & Demo")) {
                        VStack(spacing: 12) {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(tr("Тема оформлення", "Appearance"))
                                        .font(.system(size: 13.5, weight: .medium))
                                        .foregroundColor(s.text)
                                    Text(tr("Нейтральний стиль LanVid Studio", "Neutral LanVid Studio palette"))
                                        .font(.system(size: 11.5))
                                        .foregroundColor(s.text3)
                                }
                                Spacer()
                                StudioSegmentedControl(
                                    items: StudioThemeMode.allCases,
                                    selection: $engine.themeMode,
                                    title: { $0 == .dark ? tr("Темна", "Dark") : tr("Світла", "Light") },
                                    compact: true
                                )
                                .frame(width: 152)
                            }

                            Divider().overlay(s.border)

                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(tr("Demo Cine Studio", "Demo Cine Studio"))
                                        .font(.system(size: 13.5, weight: .medium))
                                        .foregroundColor(s.text)
                                    Text(tr("Інтерактивний видошукач, жести та телеметрія без ПК", "Interactive viewfinder, touch gestures & telemetry without PC"))
                                        .font(.system(size: 11.5))
                                        .foregroundColor(s.text3)
                                }
                                Spacer()
                                if engine.isDemoMode {
                                    Button(tr("Вийти", "Exit")) {
                                        engine.exitDemoMode()
                                        dismiss()
                                    }
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundColor(.white)
                                    .padding(.horizontal, 12)
                                    .frame(height: 30)
                                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm).fill(s.red))
                                } else {
                                    Button(tr("Запустити", "Launch")) {
                                        engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                                        dismiss()
                                    }
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundColor(s.primaryFg)
                                    .padding(.horizontal, 12)
                                    .frame(height: 30)
                                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm).fill(s.primaryBg))
                                }
                            }
                        }
                    }

                    // Section 2: Multi-Level Stabilization
                    sectionBlock(title: tr("Стабілізація зображення", "Image Stabilization")) {
                        VStack(alignment: .leading, spacing: 10) {
                            StudioSegmentedControl(
                                items: StabilizationLevel.allCases,
                                selection: Binding(
                                    get: { engine.stabilizationLevel },
                                    set: { engine.setStabilizationLevel($0) }
                                ),
                                title: { $0.title },
                                compact: true
                            )
                            HStack {
                                Text(engine.stabilizationLevel.subtitle)
                                    .font(.system(size: 11.5))
                                    .foregroundColor(s.text2)
                                Spacer()
                                StudioBadge(
                                    text: tr("Апаратно: ", "HW: ") + engine.activeHardwareStabilization.title,
                                    tone: engine.activeHardwareStabilization == .off ? .neutral : .green
                                )
                            }
                        }
                    }

                    // Section 3: Touch Gestures Reference
                    sectionBlock(title: tr("Жести на екрані видошукача", "Viewfinder Touch Gestures")) {
                        VStack(spacing: 9) {
                            gestureHelpRow(badge: tr("Тап", "Tap"), desc: tr("Фокус і замір експозиції в точці дотику", "Focus & exposure metering at tapped point"))
                            Divider().overlay(s.border)
                            gestureHelpRow(badge: tr("Свайп ↑↓", "Drag ↑↓"), desc: tr("Плавна корекція експозиції (−3.0 … +3.0 EV)", "Smooth exposure compensation (−3.0 … +3.0 EV)"))
                            Divider().overlay(s.border)
                            gestureHelpRow(badge: tr("Свайп ←→", "Drag ←→"), desc: tr("Точний ручний фокус (Manual Focus Pull)", "Precision manual focus pull (0–100%)"))
                            Divider().overlay(s.border)
                            gestureHelpRow(badge: tr("Щипок", "Pinch"), desc: tr("Безшовний оптичний і цифровий зум (0.5×–10×)", "Seamless optical & digital zoom (0.5×–10×)"))
                            Divider().overlay(s.border)
                            gestureHelpRow(badge: tr("Утримання", "Hold"), desc: tr("Блокування AE / AF у вибраній точці", "Lock AE / AF at selected point"))
                            Divider().overlay(s.border)
                            gestureHelpRow(badge: tr("2× Тап", "2× Tap"), desc: tr("Миттєве скидання в автофокус, AE та 0.0 EV", "Reset to Auto Focus, Auto Exposure & 0.0 EV"))
                        }
                    }

                    // Section 4: Studio Connection
                    sectionBlock(title: tr("Підключення LanVid Studio", "LanVid Studio Connection")) {
                        VStack(spacing: 10) {
                            labeledField(tr("Назва камери", "Camera Label"), text: $engine.cameraLabel)
                            Divider().overlay(s.border)
                            labeledField(tr("IP-адреса", "Studio IP"), text: $engine.destIp, decimal: true)
                            Divider().overlay(s.border)
                            Stepper(value: $engine.videoPort, in: 1024...65535) {
                                HStack {
                                    Text(tr("Порт відео (UDP)", "Video Port (UDP)"))
                                        .font(.system(size: 13)).foregroundColor(s.text)
                                    Spacer()
                                    Text("\(engine.videoPort)")
                                        .font(.system(size: 13, weight: .semibold, design: .monospaced))
                                        .foregroundColor(s.text2)
                                }
                            }
                            Divider().overlay(s.border)
                            Stepper(value: $engine.tallyPort, in: 1024...65535) {
                                HStack {
                                    Text(tr("Порт Tally / Інтерком", "Tally / Intercom Port"))
                                        .font(.system(size: 13)).foregroundColor(s.text)
                                    Spacer()
                                    Text("\(engine.tallyPort)")
                                        .font(.system(size: 13, weight: .semibold, design: .monospaced))
                                        .foregroundColor(s.text2)
                                }
                            }
                        }
                    }

                    // Section 5: Assistants
                    sectionBlock(title: tr("Асистенти кадру", "Viewfinder Assistants")) {
                        VStack(spacing: 8) {
                            Toggle(tr("Сітка кадрування та межі EIS", "Framing Guides & EIS Safe Frame"), isOn: $engine.showGuides).tint(s.accent)
                            Divider().overlay(s.border)
                            Toggle(tr("Фокус-пікінг", "Focus Peaking"), isOn: $engine.focusPeaking).tint(s.accent)
                            Divider().overlay(s.border)
                            Toggle(tr("Зебра пересвітів", "Exposure Zebra"), isOn: $engine.zebra).tint(s.accent)
                            Divider().overlay(s.border)
                            Toggle(tr("Гістограма та VU-метри", "Histogram & VU Meters"), isOn: $engine.showHistogram).tint(s.accent)
                        }
                        .font(.system(size: 13))
                        .foregroundColor(s.text)
                    }

                    // Section 6: Privacy & Support
                    sectionBlock(title: tr("Про застосунок", "About & Support")) {
                        VStack(spacing: 10) {
                            Link(destination: AppLinks.privacyPolicy) {
                                HStack {
                                    Label(tr("Політика конфіденційності", "Privacy Policy"), systemImage: "hand.raised")
                                        .font(.system(size: 13)).foregroundColor(s.text)
                                    Spacer()
                                    Image(systemName: "arrow.up.right").font(.system(size: 11)).foregroundColor(s.text3)
                                }
                            }
                            Divider().overlay(s.border)
                            Link(destination: AppLinks.support) {
                                HStack {
                                    Label(tr("Підтримка та інструкція", "Support & Help"), systemImage: "questionmark.circle")
                                        .font(.system(size: 13)).foregroundColor(s.text)
                                    Spacer()
                                    Image(systemName: "arrow.up.right").font(.system(size: 11)).foregroundColor(s.text3)
                                }
                            }
                            Divider().overlay(s.border)
                            HStack {
                                Text(tr("Версія", "Version")).font(.system(size: 13)).foregroundColor(s.text)
                                Spacer()
                                Text("2.7.0 · Local LAN Only")
                                    .font(.system(size: 11.5, design: .monospaced))
                                    .foregroundColor(s.text3)
                            }
                        }
                    }
                }
                .padding(16)
            }
            .background(s.bg.ignoresSafeArea())
            .navigationTitle(tr("Налаштування", "Settings"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(tr("Готово", "Done")) { dismiss() }
                        .foregroundColor(s.accent)
                }
            }
        }
        .preferredColorScheme(engine.themeMode == .dark ? .dark : .light)
        .onDisappear {
            engine.reconnectIfEndpointChanged()
        }
    }

    private func sectionBlock<Content: View>(title: String, @ViewBuilder content: @escaping () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title)
                .font(.system(size: 12.5, weight: .semibold))
                .foregroundColor(s.text2)
                .padding(.leading, 4)
            StudioCard(padding: 14, content: content)
        }
    }

    private func gestureHelpRow(badge: String, desc: String) -> some View {
        HStack(spacing: 10) {
            Text(badge)
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(s.text)
                .frame(width: 78)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(s.surface2))
            Text(desc)
                .font(.system(size: 12.5))
                .foregroundColor(s.text2)
            Spacer(minLength: 0)
        }
    }

    private func labeledField(_ label: String, text: Binding<String>, decimal: Bool = false) -> some View {
        HStack {
            Text(label)
                .font(.system(size: 13))
                .foregroundColor(s.text)
            Spacer()
            TextField(label, text: text)
                .multilineTextAlignment(.trailing)
                .font(.system(size: 13, design: decimal ? .monospaced : .default))
                .foregroundColor(s.text)
                .keyboardType(decimal ? .decimalPad : .default)
        }
    }
}
