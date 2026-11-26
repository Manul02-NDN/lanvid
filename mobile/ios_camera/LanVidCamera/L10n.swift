import Foundation

/// Bilingual English / Ukrainian helper. Follows device locale unless overridden via `-LVLang uk|en`.
enum L10n {
    static let isUkrainian: Bool = {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "-LVLang"), i + 1 < args.count {
            return args[i + 1].lowercased().hasPrefix("uk")
        }
        return Locale.preferredLanguages.first?.lowercased().hasPrefix("uk") ?? false
    }()
}

func tr(_ uk: String, _ en: String) -> String {
    L10n.isUkrainian ? uk : en
}

enum AppLinks {
    static let privacyPolicy = URL(string: "https://manul02-ndn.github.io/lanvid/camera-privacy.html")!
    static let support = URL(string: "https://manul02-ndn.github.io/lanvid/support.html")!
}
