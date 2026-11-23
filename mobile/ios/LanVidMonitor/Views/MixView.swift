import SwiftUI

struct MixView: View {
    @EnvironmentObject var engine: MonitorEngine

    var body: some View {
        VStack(spacing: 12) {
            Card(title: tr("Моніторинговий стереопотік", "Monitor stereo stream")) {
                VStack(alignment: .leading, spacing: 8) {
                    Text(tr("Якість", "Quality")).font(.caption).foregroundStyle(Theme.muted)
                    SegmentRow(items: StreamQuality.allCases, selection: $engine.quality) { $0.label }
                    Text(engine.quality.detail).font(.caption2).foregroundStyle(Theme.muted)
                    Text(tr("Буфер", "Buffer")).font(.caption).foregroundStyle(Theme.muted)
                    SegmentRow(items: MonitorBuffer.allCases, selection: $engine.buffer) { "\($0.rawValue) " + tr("мс", "ms") }
                }
            }
            if engine.connected == nil {
                Spacer()
                Text(tr("Підключіться до LanVid Studio на вкладці «Підключення» або відкрийте Demo Studio.",
                         "Connect to LanVid Studio on the Connect tab, or open the Demo Studio."))
                    .foregroundStyle(Theme.muted).multilineTextAlignment(.center).padding()
                Spacer()
            } else if engine.channels.isEmpty {
                Spacer()
                ProgressView(tr("Отримання каналів…", "Receiving channels…")).tint(Theme.accent2).foregroundStyle(Theme.muted)
                Spacer()
            } else {
                ScrollView(.horizontal, showsIndicators: true) {
                    HStack(alignment: .top, spacing: 8) {
                        ForEach(engine.channels) { ch in
                            ChannelStripView(strip: ch, anySolo: engine.channels.contains { $0.solo }) { engine.update($0) }
                        }
                    }
                    .padding(.horizontal)
                }
                HStack {
                    Text(tr("Мікс виконується на ПК — у телефон надходить готове стерео.", "Mixing runs on the PC — the phone receives the finished stereo."))
                        .font(.caption2).foregroundStyle(Theme.muted)
                    Spacer()
                    Button(tr("Скинути", "Reset")) { engine.resetMix() }.font(.caption).tint(Theme.accent2)
                }
                .padding(.horizontal)
            }
        }
        .padding(.vertical)
        .background(Theme.bg.ignoresSafeArea())
    }
}

struct ChannelStripView: View {
    var strip: ChannelStrip
    var anySolo: Bool
    var onChange: (ChannelStrip) -> Void

    var body: some View {
        let audible = !strip.mute && (!anySolo || strip.solo)
        VStack(spacing: 8) {
            Text(strip.name).font(.caption.weight(.semibold)).foregroundStyle(Theme.text)
                .lineLimit(1).frame(width: 76)
            HStack(alignment: .bottom, spacing: 6) {
                VUMeter(dbfs: strip.peakDbfs).frame(width: 6, height: 190)
                VerticalFader(valueDb: binding(\.gainDb), tint: audible ? Theme.accent : Theme.muted)
            }
            Text(String(format: "%+.1f", strip.gainDb)).font(.caption2.monospacedDigit()).foregroundStyle(Theme.muted)
            PanControl(pan: binding(\.pan)).frame(width: 76)
            HStack(spacing: 6) {
                PillToggle(label: "M", on: strip.mute, color: Theme.red) { var s = strip; s.mute.toggle(); onChange(s) }
                PillToggle(label: "S", on: strip.solo, color: Theme.amber) { var s = strip; s.solo.toggle(); onChange(s) }
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 14).fill(Theme.card))
        .opacity(audible ? 1 : 0.6)
    }

    private func binding(_ kp: WritableKeyPath<ChannelStrip, Float>) -> Binding<Float> {
        Binding(get: { strip[keyPath: kp] }, set: { v in var s = strip; s[keyPath: kp] = v; onChange(s) })
    }
}
