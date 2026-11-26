import SwiftUI

@main
struct LanVidCameraApp: App {
    @StateObject private var engine = CameraEngine()

    var body: some Scene {
        WindowGroup {
            ProCineHUDView()
                .environmentObject(engine)
                .preferredColorScheme(.dark)
                .onAppear {
                    if !CommandLine.arguments.contains("-LVDemo") && !CommandLine.arguments.contains("-LVShot") {
                        engine.startDiscovery()
                    }
                }
        }
    }
}
