import SwiftUI

@main
struct LanVidMonitorApp: App {
    @StateObject private var engine = MonitorEngine()
    @State private var tab: Int

    init() {
        // Launch arguments for UI tests / screenshots:  -LVDemo  (open the Demo Studio)  -LVTab 0..3
        let args = CommandLine.arguments
        var start = 0
        if let i = args.firstIndex(of: "-LVTab"), i + 1 < args.count, let t = Int(args[i + 1]) { start = min(max(t, 0), 3) }
        _tab = State(initialValue: start)
    }

    var body: some Scene {
        WindowGroup {
            TabView(selection: $tab) {
                ConnectView().tag(0)
                    .tabItem { Label(tr("Підключення", "Connect"), systemImage: "wifi") }
                MixView().tag(1)
                    .tabItem { Label(tr("Мікшер", "Mixer"), systemImage: "slider.vertical.3") }
                FxView().tag(2)
                    .tabItem { Label("Monitor FX", systemImage: "waveform.path.ecg") }
                SettingsView().tag(3)
                    .tabItem { Label(tr("Налаштування", "Settings"), systemImage: "gearshape") }
            }
            .tint(Theme.accent2)
            .environmentObject(engine)
            .preferredColorScheme(.dark)
            .onAppear {
                if CommandLine.arguments.contains("-LVDemo") {
                    engine.startDemo()
                    if CommandLine.arguments.contains("-LVMarketing") { engine.applyMarketingPreset() }
                }
            }
        }
    }
}
