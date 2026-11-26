import SwiftUI

/// Selector for all 3 iPhone Duo modes (Single HUD, Dual-Cam PIP / 50-50 Split via AVCaptureMultiCamSession,
/// and paired iPhone Duo Sync: CAM A + CAM B / Director Companion).
struct DuoSyncSheetView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section(tr("Режим iPhone Duo", "iPhone Duo Layout Mode")) {
                    ForEach(DuoMode.allCases) { mode in
                        Button {
                            engine.setDuoMode(mode)
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: icon(for: mode))
                                    .font(.title3)
                                    .foregroundColor(engine.duoMode == mode ? CineTheme.lavenderBright : CineTheme.textMuted)
                                    .frame(width: 28)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(mode.title)
                                        .font(.headline)
                                        .foregroundColor(CineTheme.textPrimary)
                                    Text(mode.subtitle)
                                        .font(.caption)
                                        .foregroundColor(CineTheme.textMuted)
                                }
                                Spacer()
                                if engine.duoMode == mode {
                                    Image(systemName: "checkmark.circle.fill")
                                        .foregroundColor(CineTheme.lavenderBright)
                                }
                            }
                            .padding(.vertical, 4)
                        }
                    }
                    HStack {
                        Text("AVCaptureMultiCamSession")
                            .font(.caption)
                            .foregroundColor(CineTheme.textMuted)
                        Spacer()
                        Text(engine.multiCamSupported ? tr("Підтримується", "Supported") : tr("Демо / Симуляція", "Demo / Simulated"))
                            .font(.caption.monospaced())
                            .foregroundColor(engine.multiCamSupported ? CineTheme.tallyGreen : CineTheme.amber)
                    }
                }

                Section(tr("Роль цього iPhone у зв'язці Duo", "This iPhone's Duo Role")) {
                    Picker(tr("Роль", "Role"), selection: $engine.duoRole) {
                        ForEach(DuoRole.allCases) { r in
                            Text(r.displayName).tag(r)
                        }
                    }
                    .pickerStyle(.segmented)
                    .onChange(of: engine.duoRole) { newRole in
                        if newRole == .companion {
                            engine.duoMode = .duoCompanion
                        } else {
                            engine.cameraLabel = newRole.rawValue
                        }
                        engine.syncDuoStateToPeers()
                    }
                }

                Section(tr("Знайдені сусідні iPhone Duo (Wi-Fi)", "Discovered Paired iPhone Duo Peers")) {
                    if engine.browser.duoPeers.isEmpty {
                        Text(tr("Відкрийте LanVid Camera на другому iPhone у тій самій Wi-Fi мережі — він з'явиться автоматично.",
                                "Open LanVid Camera on a second iPhone on the same Wi-Fi — it will appear automatically."))
                            .font(.footnote)
                            .foregroundColor(CineTheme.textMuted)
                    } else {
                        ForEach(engine.browser.duoPeers) { peer in
                            HStack {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text("\(peer.name) (\(peer.role.rawValue))")
                                        .font(.subheadline.bold())
                                    Text("\(peer.host) • TC \(peer.timecode) • ISO \(Int(peer.isoValue))")
                                        .font(.caption.monospaced())
                                        .foregroundColor(CineTheme.textMuted)
                                }
                                Spacer()
                                Text(peer.tally.badgeText)
                                    .font(.caption2.bold().monospaced())
                                    .padding(.horizontal, 8)
                                    .padding(.vertical, 4)
                                    .background(peer.tally.accentColor)
                                    .foregroundColor(.white)
                                    .clipShape(Capsule())
                            }
                            .contentShape(Rectangle())
                            .onTapGesture {
                                engine.selectedDuoPeerHost = peer.host
                                engine.syncDuoStateToPeers()
                            }
                        }
                    }
                    Button(tr("Синхронізувати таймкод та Tally", "Sync Timecode & Tally Now")) {
                        engine.syncDuoStateToPeers()
                    }
                    .tint(CineTheme.lavenderBright)
                }
            }
            .navigationTitle("iPhone Duo")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(tr("Готово", "Done")) { dismiss() }
                }
            }
        }
        .preferredColorScheme(.dark)
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
