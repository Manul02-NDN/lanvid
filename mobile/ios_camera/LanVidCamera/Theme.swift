import SwiftUI

/// Dark Studio Glass (#12101B -> #211A36) + Soft Pastel Lavender (#DDD6FE / #8B5CF6)
enum CineTheme {
    static let bgDarkTop = Color(red: 0.071, green: 0.063, blue: 0.106)       // #12101B
    static let bgDarkBot = Color(red: 0.129, green: 0.102, blue: 0.212)       // #211A36
    static let glass = Color(red: 0.095, green: 0.082, blue: 0.155).opacity(0.84)
    static let glassLight = Color(red: 0.165, green: 0.140, blue: 0.265).opacity(0.76)
    static let border = Color(red: 0.867, green: 0.839, blue: 0.996).opacity(0.24)  // #DDD6FE
    static let lavenderSoft = Color(red: 0.867, green: 0.839, blue: 0.996)    // #DDD6FE
    static let lavender = Color(red: 0.545, green: 0.361, blue: 0.965)        // #8B5CF6
    static let lavenderBright = Color(red: 0.655, green: 0.545, blue: 0.980)  // #A78BFA
    static let tallyRed = Color(red: 0.937, green: 0.267, blue: 0.267)        // #EF4444
    static let tallyGreen = Color(red: 0.063, green: 0.725, blue: 0.506)      // #10B981
    static let amber = Color(red: 0.961, green: 0.620, blue: 0.043)           // #F59E0B
    static let slate = Color(red: 0.45, green: 0.49, blue: 0.58)
    static let textPrimary = Color(red: 0.96, green: 0.95, blue: 0.99)
    static let textMuted = Color(red: 0.73, green: 0.70, blue: 0.84)

    static var backdropGradient: LinearGradient {
        LinearGradient(colors: [bgDarkTop, bgDarkBot], startPoint: .topLeading, endPoint: .bottomTrailing)
    }
}
