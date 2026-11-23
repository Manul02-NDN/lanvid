import SwiftUI

/// Monitor FX — Server DSP: everything here runs in real time on the PC on this phone's personal bus.
struct FxView: View {
    @EnvironmentObject var engine: MonitorEngine

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                Text(tr("Обробка виконується на ПК у реальному часі лише для вашої моніторної шини.", "Processing runs in real time on the PC, on your personal monitor bus only."))
                    .font(.caption).foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading)

                Card(title: "High-Pass Filter") {
                    SegmentRow(items: HpfMode.allCases, selection: $engine.dsp.hpf) { $0.label }
                }

                Card(title: tr("3-смуговий еквалайзер", "3-band equalizer"),
                     trailing: AnyView(Toggle("", isOn: $engine.dsp.eqOn).labelsHidden().tint(Theme.accent))) {
                    ParamSlider(title: "Low (100 " + tr("Гц", "Hz") + ")", value: $engine.dsp.eqLowDb, range: -12...12)
                    ParamSlider(title: "Mid", value: $engine.dsp.eqMidDb, range: -12...12)
                    HStack(spacing: 6) {
                        ForEach(DspSettings.midFrequencies, id: \.self) { f in
                            Button { engine.dsp.eqMidHz = f } label: {
                                Text(f >= 1000 ? String(format: "%.1fk", f / 1000) : "\(Int(f))")
                                    .font(.caption.weight(.semibold)).frame(maxWidth: .infinity, minHeight: 28)
                                    .foregroundStyle(engine.dsp.eqMidHz == f ? Color.white : Theme.text)
                                    .background(RoundedRectangle(cornerRadius: 8)
                                        .fill(engine.dsp.eqMidHz == f ? Theme.accent : Theme.cardHi))
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    ParamSlider(title: "High (8 " + tr("кГц", "kHz") + ")", value: $engine.dsp.eqHighDb, range: -12...12)
                }
                .opacity(engine.dsp.eqOn ? 1 : 0.55)

                Card(title: tr("Компресор", "Compressor"),
                     trailing: AnyView(Toggle("", isOn: $engine.dsp.compOn).labelsHidden().tint(Theme.accent))) {
                    ParamSlider(title: "Threshold", value: $engine.dsp.compThresholdDb, range: -40...0)
                    ParamSlider(title: "Ratio", value: $engine.dsp.compRatio, range: 1...10, step: 0.1, format: "%.1f : 1")
                    ParamSlider(title: "Makeup Gain", value: $engine.dsp.compMakeupDb, range: 0...18)
                    GainReductionMeter(grDb: engine.dsp.compOn ? engine.telemetry.gainReductionDb : 0)
                }
                .opacity(engine.dsp.compOn ? 1 : 0.55)

                Card(title: "Master Limiter & Volume") {
                    HStack {
                        Image(systemName: "shield.lefthalf.filled").foregroundStyle(Theme.green)
                        Text(tr("Лімітер −0.5 dBFS завжди увімкнений — захищає слух від різких піків.", "The −0.5 dBFS limiter is always on — it protects your hearing from sudden peaks."))
                            .font(.caption).foregroundStyle(Theme.muted)
                    }
                    ParamSlider(title: tr("Гучність у навушниках", "Headphone volume"), value: $engine.dsp.masterVolumeDb, range: -60...12)
                    HStack {
                        Text(tr("Вихід", "Output")).font(.caption).foregroundStyle(Theme.muted)
                        VUMeter(dbfs: engine.telemetry.mixPeakDbfs, vertical: false).frame(height: 8)
                        Text(String(format: "%.0f dBFS", engine.telemetry.mixPeakDbfs))
                            .font(.caption2.monospacedDigit()).foregroundStyle(Theme.muted).frame(width: 64, alignment: .trailing)
                    }
                    Toggle(tr("Mute (вимкнути звук)", "Mute"), isOn: $engine.dsp.masterMute).tint(Theme.red)
                        .foregroundStyle(Theme.text)
                }

                Button(tr("Скинути обробку", "Reset processing")) { engine.dsp = DspSettings() }
                    .font(.footnote).tint(Theme.accent2)
            }
            .padding()
        }
        .background(Theme.bg.ignoresSafeArea())
        .disabled(engine.connected == nil)
    }
}

struct GainReductionMeter: View {
    var grDb: Float  // <= 0
    var body: some View {
        HStack {
            Text("Gain Reduction").font(.caption).foregroundStyle(Theme.muted)
            GeometryReader { g in
                let t = CGFloat(min(max(-grDb / 24, 0), 1))
                ZStack(alignment: .trailing) {
                    RoundedRectangle(cornerRadius: 3).fill(Theme.cardHi)
                    RoundedRectangle(cornerRadius: 3).fill(Theme.amber).frame(width: g.size.width * t)
                }
                .animation(.linear(duration: 0.08), value: grDb)
            }
            .frame(height: 8)
            Text(String(format: tr("%.1f дБ", "%.1f dB"), grDb)).font(.caption2.monospacedDigit()).foregroundStyle(Theme.amber)
                .frame(width: 56, alignment: .trailing)
        }
    }
}
