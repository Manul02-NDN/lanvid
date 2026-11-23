import Foundation

/// English (default) / Ukrainian UI strings. The language follows the device language (Ukrainian when it is
/// the first preferred language); `-LVLang uk|en` on the command line overrides it (screenshots, tests).
enum L10n {
    static let isUkrainian: Bool = {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "-LVLang"), i + 1 < args.count { return args[i + 1].hasPrefix("uk") }
        return Locale.preferredLanguages.first?.hasPrefix("uk") ?? false
    }()
}

/// Picks the Ukrainian or the English variant of a UI string.
func tr(_ uk: String, _ en: String) -> String { L10n.isUkrainian ? uk : en }

/// Public pages published from docs/ through GitHub Pages (workflow pages.yml).
enum AppLinks {
    static let privacyPolicy = URL(string: "https://manul02-ndn.github.io/lanvid/privacy-policy.html")!
    static let support = URL(string: "https://manul02-ndn.github.io/lanvid/support.html")!
}
