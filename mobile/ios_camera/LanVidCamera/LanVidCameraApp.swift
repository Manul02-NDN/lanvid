import SwiftUI

@main
struct LanVidCameraApp: App {
    @StateObject private var engine = CameraEngine()

    var body: some Scene {
        WindowGroup {
            ProCineHUDView()
                .environmentObject(engine)
                .environment(\.studio, engine.themeMode == .dark ? .dark : .light)
                .preferredColorScheme(engine.themeMode == .dark ? .dark : .light)
                .onAppear {
                    if !CommandLine.arguments.contains("-LVDemo") && !CommandLine.arguments.contains("-LVShot") {
                        engine.startDiscovery()
                        engine.startLocalPreviewIfAuthorized()
                    }
                }
                .onOpenURL { url in
                    engine.handlePairingURL(url)
                }
        }
    }
}
