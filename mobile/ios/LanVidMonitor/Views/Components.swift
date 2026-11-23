import SwiftUI

/// Vertical fader in dB (-60 ... +12). Drag to move; double-tap resets to 0 dB.
struct VerticalFader: View {
    @Binding var valueDb: Float
    var range: ClosedRange<Float> = -60...12
    var height: CGFloat = 190
    var tint: Color = Theme.accent

    private func norm(_ db: Float) -> CGFloat {  // perceptual taper: more travel near 0 dB
        let t = (db - range.lowerBound) / (range.upperBound - range.lowerBound)
        return CGFloat(pow(Double(t), 1.6))
    }
    private func db(from n: CGFloat) -> Float {
        let t = Float(pow(Double(min(max(n, 0), 1)), 1 / 1.6))
        return (range.lowerBound + t * (range.upperBound - range.lowerBound) * 1).rounded(toPlaces: 1)
    }

    var body: some View {
        GeometryReader { g in
            let h = g.size.height
            let y = h * (1 - norm(valueDb))
            ZStack(alignment: .top) {
                Capsule().fill(Theme.cardHi).frame(width: 6).frame(maxWidth: .infinity)
                Capsule().fill(tint.opacity(0.85)).frame(width: 6, height: max(0, h - y))
                    .frame(maxWidth: .infinity).offset(y: y)
                // 0 dB mark
                Rectangle().fill(Theme.muted.opacity(0.6)).frame(width: 22, height: 1)
                    .offset(y: h * (1 - norm(0)))
                RoundedRectangle(cornerRadius: 5).fill(Theme.text)
                    .frame(width: 34, height: 16)
                    .overlay(Rectangle().fill(tint).frame(height: 2))
                    .shadow(color: .black.opacity(0.4), radius: 3, y: 1)
                    .offset(y: y - 8)
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { v in
                valueDb = db(from: 1 - v.location.y / h)
            })
            .onTapGesture(count: 2) { valueDb = 0 }
        }
        .frame(width: 44, height: height)
        .accessibilityElement()
        .accessibilityLabel(tr("Гучність", "Volume"))
        .accessibilityValue(String(format: tr("%.1f дБ", "%.1f dB"), valueDb))
        .accessibilityAdjustableAction { dir in
            valueDb = min(max(valueDb + (dir == .increment ? 1 : -1), range.lowerBound), range.upperBound)
        }
    }
}

/// Live peak meter (dBFS, -60 ... 0), vertical or horizontal, with short visual decay.
struct VUMeter: View {
    var dbfs: Float
    var vertical = true
    var body: some View {
        GeometryReader { g in
            let t = CGFloat(min(max((dbfs + 60) / 60, 0), 1))
            ZStack(alignment: vertical ? .bottom : .leading) {
                RoundedRectangle(cornerRadius: 2).fill(Theme.cardHi)
                RoundedRectangle(cornerRadius: 2)
                    .fill(LinearGradient(colors: [Theme.green, Theme.amber, Theme.red],
                                         startPoint: vertical ? .bottom : .leading, endPoint: vertical ? .top : .trailing))
                    .mask(alignment: vertical ? .bottom : .leading) {
                        Rectangle().frame(width: vertical ? nil : g.size.width * t,
                                          height: vertical ? g.size.height * t : nil)
                    }
            }
            .animation(.linear(duration: 0.07), value: dbfs)
        }
    }
}

/// Pan control (-1 L ... +1 R) with center detent; double-tap centers.
struct PanControl: View {
    @Binding var pan: Float
    var body: some View {
        VStack(spacing: 2) {
            Slider(value: Binding(get: { Double(pan) }, set: { v in
                let s = Float(v)
                pan = abs(s) < 0.06 ? 0 : (s * 100).rounded() / 100
            }), in: -1...1)
            .tint(Theme.accent2)
            .onTapGesture(count: 2) { pan = 0 }
            Text(pan == 0 ? "C" : (pan < 0 ? "L\(Int(-pan * 100))" : "R\(Int(pan * 100))"))
                .font(.caption2.monospacedDigit()).foregroundStyle(Theme.muted)
        }
    }
}

/// Small toggle button (M / S).
struct PillToggle: View {
    var label: String
    var on: Bool
    var color: Color
    var action: () -> Void
    var body: some View {
        Button(action: action) {
            Text(label).font(.caption.weight(.heavy))
                .frame(width: 30, height: 26)
                .foregroundStyle(on ? Color.black : Theme.text)
                .background(RoundedRectangle(cornerRadius: 7).fill(on ? color : Theme.cardHi))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}

/// Segmented chooser with pill look.
struct SegmentRow<T: Hashable & Identifiable>: View {
    var items: [T]
    @Binding var selection: T
    var label: (T) -> String
    var body: some View {
        HStack(spacing: 6) {
            ForEach(items) { it in
                Button { selection = it } label: {
                    Text(label(it)).font(.footnote.weight(.semibold))
                        .frame(maxWidth: .infinity, minHeight: 32)
                        .foregroundStyle(it == selection ? Color.white : Theme.text)
                        .background(RoundedRectangle(cornerRadius: 9)
                            .fill(it == selection ? Theme.accent : Theme.cardHi))
                }
                .buttonStyle(.plain)
            }
        }
    }
}

/// Labeled parameter slider with value readout.
struct ParamSlider: View {
    var title: String
    @Binding var value: Float
    var range: ClosedRange<Float>
    var step: Float = 0.5
    var format: String = tr("%.1f дБ", "%.1f dB")
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(title).font(.subheadline).foregroundStyle(Theme.text)
                Spacer()
                Text(String(format: format, value)).font(.subheadline.monospacedDigit()).foregroundStyle(Theme.accent2)
            }
            Slider(value: Binding(get: { Double(value) }, set: { value = (Float($0) / step).rounded() * step }),
                   in: Double(range.lowerBound)...Double(range.upperBound))
                .tint(Theme.accent)
        }
    }
}

extension Float {
    func rounded(toPlaces p: Int) -> Float {
        let m = Float(pow(10.0, Double(p)))
        return (self * m).rounded() / m
    }
}
