import SwiftUI

public enum BeltpackChannel: String, CaseIterable, Identifiable {
    case director = "Director"
    case groupA = "Group A"
    case groupB = "Group B"
    case allCall = "All Call"

    public var id: String { rawValue }
}

public enum BeltpackTallyState: Int {
    case standby = 0
    case preview = 1
    case program = 2

    var title: String {
        switch self {
        case .standby: return "STANDBY"
        case .preview: return "PREVIEW"
        case .program: return "ON AIR / PGM"
        }
    }

    var accentColor: Color {
        switch self {
        case .standby: return Color(red: 0.545, green: 0.361, blue: 0.965)
        case .preview: return Color(red: 0.063, green: 0.725, blue: 0.506)
        case .program: return Color(red: 0.937, green: 0.267, blue: 0.267)
        }
    }
}

/// Standalone Beltpack UI built on top of `lv_tally_intercom_*` (port 5024).
public struct BeltpackIntercomView: View {
    @State private var deskHost: String = "192.168.1.10"
    @State private var deskPort: Int = 5024
    @State private var channel: BeltpackChannel = .director
    @State private var pttPressed: Bool = false
    @State private var latched: Bool = false
    @State private var headphoneVolume: Double = 1.0
    @State private var tally: BeltpackTallyState = .standby
    @State private var connected: Bool = false

    public init() {}

    public var body: some View {
        VStack(spacing: 18) {
            // Full-width Tally / ON AIR status banner
            RoundedRectangle(cornerRadius: 18)
                .fill(tally.accentColor.opacity(0.22))
                .overlay(
                    RoundedRectangle(cornerRadius: 18)
                        .stroke(tally.accentColor, lineWidth: 3)
                )
                .frame(height: 82)
                .overlay(
                    VStack(spacing: 4) {
                        Text(tally.title)
                            .font(.system(size: 24, weight: .black, design: .rounded))
                            .foregroundColor(tally.accentColor)
                        Text(connected ? "\(deskHost):\(deskPort)" : "Disconnected")
                            .font(.caption.monospaced())
                            .foregroundColor(.secondary)
                    }
                )

            // Partyline / Director channel selector
            Picker("Intercom Channel", selection: $channel) {
                ForEach(BeltpackChannel.allCases) { ch in
                    Text(ch.rawValue).tag(ch)
                }
            }
            .pickerStyle(.segmented)

            // Tactile Hold-to-Talk (PTT) + Latch button
            Button(action: {}) {
                ZStack {
                    Circle()
                        .fill((pttPressed || latched) ? Color.red : Color.purple)
                        .frame(width: 190, height: 190)
                        .shadow(color: ((pttPressed || latched) ? Color.red : Color.purple).opacity(0.45), radius: 18)
                    VStack(spacing: 6) {
                        Image(systemName: (pttPressed || latched) ? "mic.fill" : "mic.slash.fill")
                            .font(.system(size: 40))
                        Text((pttPressed || latched) ? "TALKING" : "HOLD TO TALK")
                            .font(.headline.bold())
                    }
                    .foregroundColor(.white)
                }
            }
            .simultaneousGesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in pttPressed = true }
                    .onEnded { _ in pttPressed = false }
            )

            Toggle("LATCH (Keep Mic Open)", isOn: $latched)
                .padding(.horizontal)

            VStack(alignment: .leading, spacing: 6) {
                Text("Headphones Gain: \(Int(headphoneVolume * 100))%")
                    .font(.caption)
                Slider(value: $headphoneVolume, in: 0...2.0)
            }
            .padding(.horizontal)
        }
        .padding()
    }
}
