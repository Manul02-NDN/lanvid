import SwiftUI

/// LanVid Studio neutral design system — faithful 1:1 port of the desktop/web palette
/// (macOS Tahoe / ChatGPT & Gemini studio aesthetic: neutral charcoal surfaces, crisp hairlines,
/// single blue accent #0A84FF, tabular numerals, high-contrast primary actions, zero clutter).
enum StudioThemeMode: String, CaseIterable, Identifiable {
    case dark
    case light
    var id: String { rawValue }
}

struct StudioPalette {
    let isDark: Bool

    // Surfaces
    var bg: Color { isDark ? Color(hex: 0x1F1F1F) : Color(hex: 0xFFFFFF) }
    var bgSubtle: Color { isDark ? Color(hex: 0x181818) : Color(hex: 0xF9F9F9) }
    var surface: Color { isDark ? Color(hex: 0x262626) : Color(hex: 0xFFFFFF) }
    var surface2: Color { isDark ? Color(hex: 0x2E2E2E) : Color(hex: 0xF3F3F4) }
    var surface3: Color { isDark ? Color(hex: 0x3A3A3A) : Color(hex: 0xE8E8EA) }
    var elevated: Color { isDark ? Color(hex: 0x2C2C2C) : Color(hex: 0xFFFFFF) }
    var canvas: Color { isDark ? Color(hex: 0x1A1A1A) : Color(hex: 0xFAFAFA) }
    var segActive: Color { isDark ? Color(hex: 0x444446) : Color(hex: 0xFFFFFF) }

    // Translucent HUD surfaces over live camera video
    var hudBar: Color { isDark ? Color(hex: 0x1F1F1F).opacity(0.88) : Color(hex: 0xFFFFFF).opacity(0.90) }
    var hudSurface: Color { isDark ? Color(hex: 0x262626).opacity(0.92) : Color(hex: 0xFFFFFF).opacity(0.94) }
    var hudPill: Color { isDark ? Color(hex: 0x2E2E2E).opacity(0.92) : Color(hex: 0xF3F3F4).opacity(0.94) }

    // Borders
    var border: Color { isDark ? Color.white.opacity(0.08) : Color.black.opacity(0.08) }
    var borderStrong: Color { isDark ? Color.white.opacity(0.14) : Color.black.opacity(0.13) }
    var gridDot: Color { isDark ? Color.white.opacity(0.07) : Color.black.opacity(0.10) }

    // Typography
    var text: Color { isDark ? Color(hex: 0xECECEC) : Color(hex: 0x0D0D0D) }
    var text2: Color { isDark ? Color(hex: 0xA8A8AD) : Color(hex: 0x5D5D62) }
    var text3: Color { isDark ? Color(hex: 0x76767B) : Color(hex: 0x8F8F94) }

    // High-contrast primary button (like "+ NVA Hub")
    var primaryBg: Color { isDark ? Color(hex: 0xECECEC) : Color(hex: 0x0D0D0D) }
    var primaryFg: Color { isDark ? Color(hex: 0x121212) : Color(hex: 0xFFFFFF) }

    // Semantic accents
    var accent: Color { isDark ? Color(hex: 0x0A84FF) : Color(hex: 0x007AFF) }
    var accentSoft: Color { isDark ? Color(hex: 0x0A84FF).opacity(0.16) : Color(hex: 0x007AFF).opacity(0.10) }
    var green: Color { isDark ? Color(hex: 0x30D158) : Color(hex: 0x28B44A) }
    var greenSoft: Color { green.opacity(0.15) }
    var amber: Color { Color(hex: 0xFF9F0A) }
    var amberSoft: Color { Color(hex: 0xFF9F0A).opacity(0.15) }
    var violet: Color { isDark ? Color(hex: 0xBF5AF2) : Color(hex: 0xA550D6) }
    var violetSoft: Color { violet.opacity(0.15) }
    var red: Color { isDark ? Color(hex: 0xFF453A) : Color(hex: 0xE5372C) }
    var redSoft: Color { red.opacity(0.16) }
    var yellow: Color { Color(hex: 0xFFD60A) }

    // Corner radii (--r-sm: 9, --r-md: 13, --r-lg: 17)
    static let rSm: CGFloat = 9
    static let rMd: CGFloat = 13
    static let rLg: CGFloat = 17

    static let dark = StudioPalette(isDark: true)
    static let light = StudioPalette(isDark: false)
}

/// Environment key so every view/sheet automatically reflects dark/light studio theme.
private struct StudioPaletteKey: EnvironmentKey {
    static let defaultValue: StudioPalette = .dark
}

extension EnvironmentValues {
    var studio: StudioPalette {
        get { self[StudioPaletteKey.self] }
        set { self[StudioPaletteKey.self] = newValue }
    }
}

/// Legacy compatibility shim so existing references compile cleanly with the new neutral Studio palette.
enum CineTheme {
    static let palette = StudioPalette.dark

    static let bg = palette.bg
    static let bgSubtle = palette.bgSubtle
    static let surface = palette.surface
    static let surface2 = palette.surface2
    static let surface3 = palette.surface3
    static let elevated = palette.elevated
    static let segActive = palette.segActive
    static let border = palette.border
    static let borderStrong = palette.borderStrong

    static let accent = palette.accent
    static let accentSoft = palette.accentSoft
    static let tallyRed = palette.red
    static let tallyGreen = palette.green
    static let amber = palette.amber
    static let violet = palette.violet
    static let yellow = palette.yellow
    static let slate = palette.text3

    static let textPrimary = palette.text
    static let textSecondary = palette.text2
    static let textMuted = palette.text3

    static let glass = palette.hudSurface
    static let glassLight = palette.hudPill
}

extension Color {
    init(hex: UInt32, alpha: Double = 1.0) {
        let r = Double((hex >> 16) & 0xFF) / 255.0
        let g = Double((hex >> 8) & 0xFF) / 255.0
        let b = Double(hex & 0xFF) / 255.0
        self.init(.sRGB, red: r, green: g, blue: b, opacity: alpha)
    }
}

// MARK: - LanVid Studio Brand Mark (matches top-left circular waveform mark in screenshots)

struct LanVidBrandMark: View {
    @Environment(\.studio) private var s
    var size: CGFloat = 24

    var body: some View {
        ZStack {
            Circle()
                .fill(s.primaryBg)
                .frame(width: size, height: size)
            Image(systemName: "waveform.path.ecg")
                .font(.system(size: size * 0.54, weight: .bold))
                .foregroundColor(s.primaryFg)
        }
    }
}

// MARK: - Studio Segmented Pill Control (.seg / .tabs)

struct StudioSegmentedControl<Item: Identifiable & Hashable>: View {
    @Environment(\.studio) private var s
    let items: [Item]
    @Binding var selection: Item
    let title: (Item) -> String
    var icon: ((Item) -> String?)? = nil
    var compact: Bool = false
    var onSelect: ((Item) -> Void)? = nil

    var body: some View {
        HStack(spacing: 2) {
            ForEach(items) { item in
                let active = (selection == item)
                Button {
                    if let onSelect {
                        onSelect(item)
                    } else {
                        selection = item
                    }
                } label: {
                    HStack(spacing: 4) {
                        if let ic = icon?(item) {
                            Image(systemName: ic)
                                .font(.system(size: compact ? 10 : 12, weight: .medium))
                        }
                        Text(title(item))
                            .font(.system(size: compact ? 11 : 12.5, weight: active ? .semibold : .medium))
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                    }
                    .foregroundColor(active ? s.text : s.text2)
                    .frame(maxWidth: .infinity)
                    .frame(height: compact ? 25 : 29)
                    .padding(.horizontal, compact ? 6 : 10)
                    .background(
                        RoundedRectangle(cornerRadius: compact ? 6.5 : 7.5, style: .continuous)
                            .fill(active ? s.segActive : Color.clear)
                            .shadow(color: active ? Color.black.opacity(s.isDark ? 0.30 : 0.08) : .clear, radius: 2, y: 1)
                    )
                }
                .buttonStyle(.plain)
            }
        }
        .padding(3)
        .background(
            RoundedRectangle(cornerRadius: compact ? 9 : 10, style: .continuous)
                .fill(s.surface2)
        )
        .overlay(
            RoundedRectangle(cornerRadius: compact ? 9 : 10, style: .continuous)
                .stroke(s.border, lineWidth: 0.75)
        )
    }
}

// MARK: - Studio Status Pill (.status-pill)

struct StudioStatusPill: View {
    @Environment(\.studio) private var s
    let dotColor: Color
    let title: String
    var detail: String? = nil

    var body: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(dotColor)
                .frame(width: 7.5, height: 7.5)
                .background(
                    Circle()
                        .fill(dotColor.opacity(0.24))
                        .frame(width: 13.5, height: 13.5)
                )
            Text(title)
                .font(.system(size: 12, weight: .semibold))
                .foregroundColor(s.text)
                .lineLimit(1)
                .fixedSize()
            if let detail, !detail.isEmpty {
                Text(detail)
                    .font(.system(size: 11, weight: .medium).monospacedDigit())
                    .foregroundColor(s.text3)
                    .lineLimit(1)
            }
        }
        .padding(.leading, 10)
        .padding(.trailing, 11)
        .frame(height: 30)
        .background(Capsule().fill(s.surface2))
        .overlay(Capsule().stroke(s.border, lineWidth: 0.75))
    }
}

// MARK: - Studio Badge / Type Pill (.dev-type-pill)

enum StudioBadgeTone {
    case neutral, accent, green, amber, violet, red
}

struct StudioBadge: View {
    @Environment(\.studio) private var s
    let text: String
    var tone: StudioBadgeTone = .neutral

    var body: some View {
        let (fg, bg): (Color, Color) = {
            switch tone {
            case .neutral: return (s.text2, s.surface2)
            case .accent:  return (s.accent, s.accentSoft)
            case .green:   return (s.green, s.greenSoft)
            case .amber:   return (s.amber, s.amberSoft)
            case .violet:  return (s.violet, s.violetSoft)
            case .red:     return (s.red, s.redSoft)
            }
        }()
        return Text(text)
            .font(.system(size: 10.5, weight: .semibold))
            .foregroundColor(fg)
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 7)
            .padding(.vertical, 2.5)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(bg))
    }
}

// MARK: - Studio Preset Chip (.lat-preset-chip)

struct StudioPresetChip: View {
    @Environment(\.studio) private var s
    let title: String
    var subtitle: String? = nil
    let active: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 4) {
                Text(title)
                    .font(.system(size: 11.5, weight: .medium))
                if let subtitle {
                    Text(subtitle)
                        .font(.system(size: 10.5, weight: .medium, design: .monospaced))
                        .opacity(0.85)
                }
            }
            .foregroundColor(active ? s.accent : s.text2)
            .padding(.horizontal, 10)
            .frame(height: 26)
            .background(Capsule().fill(active ? s.accentSoft : s.surface2))
            .overlay(Capsule().stroke(active ? s.accent : s.borderStrong, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Studio Icon Button (.icon-btn)

struct StudioIconButton: View {
    @Environment(\.studio) private var s
    let systemName: String
    var active: Bool = false
    var size: CGFloat = 32
    var bordered: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: size * 0.44, weight: .medium))
                .foregroundColor(active ? s.accent : s.text2)
                .frame(width: size, height: size)
                .background(
                    RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous)
                        .fill(active ? s.accentSoft : (bordered ? s.surface : s.surface2.opacity(0.75)))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous)
                        .stroke(active ? s.accent.opacity(0.45) : (bordered ? s.borderStrong : s.border), lineWidth: 0.75)
                )
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Studio Card Container (.list / .net-kpi / .net-chart-card)

struct StudioCard<Content: View>: View {
    @Environment(\.studio) private var s
    var translucent: Bool = false
    var padding: CGFloat = 12
    @ViewBuilder let content: () -> Content

    var body: some View {
        content()
            .padding(padding)
            .background(
                RoundedRectangle(cornerRadius: StudioPalette.rLg, style: .continuous)
                    .fill(translucent ? s.hudSurface : s.surface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: StudioPalette.rLg, style: .continuous)
                    .stroke(s.borderStrong, lineWidth: 0.85)
            )
            .shadow(color: Color.black.opacity(s.isDark ? 0.28 : 0.07), radius: 14, y: 4)
    }
}

// MARK: - Studio KPI Tile (.net-kpi from Monitoring screenshot)

struct StudioKpiTile: View {
    @Environment(\.studio) private var s
    let title: String
    let trailingTag: String
    let value: String
    let unit: String
    let subtitle: String
    let progress: Double
    let barColor: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(title)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(s.text3)
                    .lineLimit(1)
                Spacer(minLength: 4)
                Text(trailingTag)
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .foregroundColor(s.text3)
                    .lineLimit(1)
            }
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value)
                    .font(.system(size: 20, weight: .semibold).monospacedDigit())
                    .foregroundColor(s.text)
                    .lineLimit(1)
                if !unit.isEmpty {
                    Text(unit)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(s.text3)
                }
            }
            Text(subtitle)
                .font(.system(size: 10.5))
                .foregroundColor(s.text2)
                .lineLimit(1)
            GeometryReader { g in
                ZStack(alignment: .leading) {
                    Capsule().fill(s.surface2)
                    Capsule()
                        .fill(barColor)
                        .frame(width: max(4, g.size.width * CGFloat(max(0.0, min(1.0, progress)))))
                }
            }
            .frame(height: 4)
            .padding(.top, 2)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
        .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))
    }
}

// MARK: - Studio Vertical VU Meter + Fader Strip (.strip from Monitoring screenshot)

struct StudioVUMeterView: View {
    @Environment(\.studio) private var s
    let dbfs: Float
    var height: CGFloat = 110

    var body: some View {
        let norm = max(0.03, min(1.0, CGFloat((dbfs + 60.0) / 60.0)))
        ZStack(alignment: .bottom) {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(s.surface3)
                .frame(width: 5, height: height)
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(
                    LinearGradient(
                        stops: [
                            .init(color: s.green, location: 0.0),
                            .init(color: s.green, location: 0.62),
                            .init(color: s.yellow, location: 0.82),
                            .init(color: s.red, location: 1.0)
                        ],
                        startPoint: .bottom,
                        endPoint: .top
                    )
                )
                .frame(width: 5, height: height)
                .mask(alignment: .bottom) {
                    Rectangle().frame(height: height * norm)
                }
                .animation(.linear(duration: 0.09), value: norm)
        }
    }
}

// MARK: - Studio Dot-Grid Canvas Background (.map-canvas)

struct StudioDotGridBackground: View {
    @Environment(\.studio) private var s

    var body: some View {
        Canvas { ctx, size in
            let step: CGFloat = 20
            var y: CGFloat = 10
            while y < size.height {
                var x: CGFloat = 10
                while x < size.width {
                    ctx.fill(
                        Path(ellipseIn: CGRect(x: x - 0.75, y: y - 0.75, width: 1.5, height: 1.5)),
                        with: .color(s.gridDot)
                    )
                    x += step
                }
                y += step
            }
        }
        .background(s.canvas)
    }
}
