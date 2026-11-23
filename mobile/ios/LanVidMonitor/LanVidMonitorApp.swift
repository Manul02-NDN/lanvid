import SwiftUI

@main
struct LanVidMonitorApp: App {
    @StateObject private var engine = MonitorEngine()

    var body: some Scene {
        WindowGroup {
            TabView {
                ConnectView().tabItem { Label("Підключення", systemImage: "wifi") }
                MixView().tabItem { Label("Channels Mix", systemImage: "slider.vertical.3") }
                FxView().tabItem { Label("Monitor FX", systemImage: "waveform.path.ecg") }
            }
            .tint(Theme.accent2)
            .environmentObject(engine)
            .preferredColorScheme(.dark)
        }
    }
}
