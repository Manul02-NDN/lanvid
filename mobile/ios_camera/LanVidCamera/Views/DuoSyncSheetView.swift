import SwiftUI

/// Selector for all iPhone Duo modes styled in the clean LanVid Studio neutral design system.
struct DuoSyncSheetView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.studio) private var s
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    // Duo Layout Mode
                    VStack(alignment: .leading, spacing: 7) {
                        Text(tr("Режим iPhone Duo", "iPhone Duo Layout Mode"))
                            .font(.system(size: 12.5, weight: .semibold))
                            .foregroundColor(s.text2)
                            .padding(.leading, 4)

                        StudioCard(padding: 0) {
                            VStack(spacing: 0) {
                                ForEach(Array(DuoMode.allCases.enumerated()), id: \.element.id) { idx, mode in
                                    if idx > 0 { Divider().overlay(s.border) }
                                    let selected = (engine.duoMode == mode)
                                    Button {
                                        engine.setDuoMode(mode)
                                    } label: {
                                        HStack(spacing: 12) {
                                            ZStack {
                                                RoundedRectangle(cornerRadius: 9, style: .continuous)
                                                    .fill(selected ? s.accentSoft : s.surface2)
                                                    .frame(width: 34, height: 34)
                                                Image(systemName: icon(for: mode))
                                                    .font(.system(size: 15, weight: .medium))
                                                    .foregroundColor(selected ? s.accent : s.text2)
                                            }
                                            VStack(alignment: .leading, spacing: 2) {
                                                Text(mode.title)
                                                    .font(.system(size: 13.5, weight: .medium))
                                                    .foregroundColor(s.text)
                                                Text(mode.subtitle)
                                                    .font(.system(size: 11.5))
                                                    .foregroundColor(s.text3)
                                                    .multilineTextAlignment(.leading)
                                            }
                                            Spacer()
                                            if selected {
                                                Image(systemName: "checkmark.circle.fill")
                                                    .foregroundColor(s.accent)
                                            }
                                        }
                                        .padding(.horizontal, 14)
                                        .padding(.vertical, 11)
                                    }
                                    .buttonStyle(.plain)
                                }

                                Divider().overlay(s.border)
                                HStack {
                                    Text("AVCaptureMultiCamSession")
                                        .font(.system(size: 12))
                                        .foregroundColor(s.text3)
                                    Spacer()
                                    StudioBadge(
                                        text: engine.multiCamSupported ? tr("Підтримується", "Supported") : tr("Демо / Симуляція", "Demo / Simulated"),
                                        tone: engine.multiCamSupported ? .green : .amber
                                    )
                                }
                                .padding(.horizontal, 14)
                                .padding(.vertical, 10)
                            }
                        }
                    }

                    // Role Selector
                    VStack(alignment: .leading, spacing: 7) {
                        Text(tr("Роль цього iPhone у зв'язці Duo", "This iPhone's Duo Role"))
                            .font(.system(size: 12.5, weight: .semibold))
                            .foregroundColor(s.text2)
                            .padding(.leading, 4)

                        StudioCard(padding: 12) {
                            StudioSegmentedControl(
                                items: DuoRole.allCases,
                                selection: $engine.duoRole,
                                title: { $0.displayName },
                                onSelect: { newRole in
                                    engine.duoRole = newRole
                                    if newRole == .companion {
                                        engine.duoMode = .duoCompanion
                                    } else {
                                        engine.cameraLabel = newRole.rawValue
                                    }
                                    engine.syncDuoStateToPeers()
                                }
                            )
                        }
                    }

                    // Discovered Peer iPhones
                    VStack(alignment: .leading, spacing: 7) {
                        Text(tr("Сусідні iPhone Duo (Wi-Fi)", "Discovered Paired iPhone Duo Peers"))
                            .font(.system(size: 12.5, weight: .semibold))
                            .foregroundColor(s.text2)
                            .padding(.leading, 4)

                        StudioCard(padding: 12) {
                            VStack(alignment: .leading, spacing: 10) {
                                if engine.browser.duoPeers.isEmpty {
                                    Text(tr("Відкрийте LanVid Camera на другому iPhone у тій самій Wi-Fi мережі — він з'явиться автоматично.",
                                            "Open LanVid Camera on a second iPhone on the same Wi-Fi — it will appear automatically."))
                                        .font(.system(size: 12))
                                        .foregroundColor(s.text3)
                                } else {
                                    ForEach(Array(engine.browser.duoPeers.enumerated()), id: \.element.id) { idx, peer in
                                        if idx > 0 { Divider().overlay(s.border) }
                                        HStack {
                                            VStack(alignment: .leading, spacing: 2) {
                                                Text("\(peer.name) (\(peer.role.rawValue))")
                                                    .font(.system(size: 13, weight: .semibold))
                                                    .foregroundColor(s.text)
                                                Text("\(peer.host) · TC \(peer.timecode) · ISO \(Int(peer.isoValue))")
                                                    .font(.system(size: 11, design: .monospaced))
                                                    .foregroundColor(s.text3)
                                            }
                                            Spacer()
                                            StudioBadge(
                                                text: peer.tally.badgeText,
                                                tone: peer.tally == .program ? .red : (peer.tally == .preview ? .green : .neutral)
                                            )
                                        }
                                        .contentShape(Rectangle())
                                        .onTapGesture {
                                            engine.selectedDuoPeerHost = peer.host
                                            engine.syncDuoStateToPeers()
                                        }
                                    }
                                }

                                Button {
                                    engine.syncDuoStateToPeers()
                                } label: {
                                    Text(tr("Синхронізувати таймкод та Tally", "Sync Timecode & Tally Now"))
                                        .font(.system(size: 12.5, weight: .semibold))
                                        .foregroundColor(s.primaryFg)
                                        .frame(maxWidth: .infinity)
                                        .frame(height: 36)
                                        .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.primaryBg))
                                }
                                .buttonStyle(.plain)
                            }
                        }
                    }
                }
                .padding(16)
            }
            .background(s.bg.ignoresSafeArea())
            .navigationTitle("iPhone Duo")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(tr("Готово", "Done")) { dismiss() }
                        .foregroundColor(s.accent)
                }
            }
        }
        .preferredColorScheme(engine.themeMode == .dark ? .dark : .light)
    }

    private func icon(for mode: DuoMode) -> String {
        switch mode {
        case .singleHUD: return "iphone"
        case .dualCamPIP: return "pip.fill"
        case .dualCamSplit: return "rectangle.split.2x1.fill"
        case .duoCompanion: return "slider.horizontal.below.rectangle"
        }
    }
}
