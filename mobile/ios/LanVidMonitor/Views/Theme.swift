import SwiftUI

/// Soft lavender look matching LanVid Studio.
enum Theme {
    static let bg = Color(red: 0.07, green: 0.06, blue: 0.13)
    static let card = Color(red: 0.12, green: 0.11, blue: 0.21)
    static let cardHi = Color(red: 0.17, green: 0.15, blue: 0.29)
    static let accent = Color(red: 0.55, green: 0.49, blue: 0.96)   // #8B7CF6
    static let accent2 = Color(red: 0.65, green: 0.55, blue: 0.98)  // #A78BFA
    static let text = Color(red: 0.95, green: 0.95, blue: 0.97)
    static let muted = Color(red: 0.60, green: 0.58, blue: 0.70)
    static let green = Color(red: 0.06, green: 0.73, blue: 0.51)
    static let amber = Color(red: 0.96, green: 0.62, blue: 0.04)
    static let red = Color(red: 0.94, green: 0.27, blue: 0.27)
    static let cyan = Color(red: 0.00, green: 0.82, blue: 1.00)

    static func meterColor(_ dbfs: Float) -> Color {
        dbfs > -3 ? red : (dbfs > -12 ? amber : green)
    }
}

struct Card<Content: View>: View {
    var title: String
    var trailing: AnyView? = nil
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(title).font(.caption.weight(.bold)).foregroundStyle(Theme.accent2).textCase(.uppercase)
                Spacer()
                if let trailing { trailing }
            }
            content
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 16).fill(Theme.card))
    }
}
