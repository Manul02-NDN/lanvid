// LanVid Virtual Camera.app - installs / removes the LanVid CoreMediaIO Camera Extension
// ("LanVid Cam 1..8" + "LanVid Program Out"). Must live in /Applications.
//   open -a "LanVid Virtual Camera" --args --install     (used by LanVid Video -> "Register virtual cameras")
//   open -a "LanVid Virtual Camera" --args --uninstall
import AppKit
import SystemExtensions

private let kExtensionID = "com.manul02.lanvid.vcam.extension"

final class AppDelegate: NSObject, NSApplicationDelegate, OSSystemExtensionRequestDelegate {
    private var window: NSWindow!
    private let status = NSTextField(wrappingLabelWithString: "")
    private var quitWhenDone = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 230),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "LanVid Virtual Camera"
        window.center()
        let title = NSTextField(labelWithString: "Віртуальні камери LanVid Cam 1–8 + LanVid Program Out")
        title.font = .boldSystemFont(ofSize: 14)
        let hint = NSTextField(wrappingLabelWithString:
            "Кожна камера з LanVid Video з'являється як окрема камера macOS (FaceTime, Zoom, Teams, OBS, браузери).")
        let install = NSButton(title: "Встановити віртуальні камери", target: self, action: #selector(installClicked))
        install.bezelStyle = .rounded
        install.keyEquivalent = "\r"
        let remove = NSButton(title: "Видалити", target: self, action: #selector(removeClicked))
        remove.bezelStyle = .rounded
        status.stringValue = "Готово до встановлення."
        let buttons = NSStackView(views: [install, remove])
        let stack = NSStackView(views: [title, hint, buttons, status])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView = NSView()
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor),
        ])
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        let args = CommandLine.arguments
        if args.contains("--install") { submit(activate: true) }
        if args.contains("--uninstall") { submit(activate: false) }
        quitWhenDone = args.contains("--quit-when-done")
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @objc private func installClicked() { submit(activate: true) }
    @objc private func removeClicked() { submit(activate: false) }

    private func submit(activate: Bool) {
        let req = activate
            ? OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: kExtensionID, queue: .main)
            : OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: kExtensionID, queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
        status.stringValue = activate ? "Встановлення розширення камери…" : "Видалення розширення камери…"
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status.stringValue = "Потрібен дозвіл: Системні параметри → Загальні → Об'єкти входу та розширення → " +
            "Розширення камери → увімкніть «LanVid Virtual Camera»."
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        status.stringValue = result == .completed
            ? "Готово. LanVid Cam 1–8 і LanVid Program Out доступні в усіх застосунках."
            : "Готово — завершиться після перезапуску Mac."
        if quitWhenDone { NSApp.terminate(nil) }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status.stringValue = "Помилка: \(error.localizedDescription)\nЗастосунок має бути в /Applications."
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
