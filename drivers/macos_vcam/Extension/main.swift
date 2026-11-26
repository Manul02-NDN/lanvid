// LanVid Camera Extension - CoreMediaIO system extension exposing "LanVid Cam 1..8" and
// "LanVid Program Out" to every macOS app (FaceTime, Zoom, Teams, Meet in Safari/Chrome, OBS, QuickTime).
import CoreMediaIO
import Foundation

let providerSource = LanVidProviderSource(clientQueue: nil)
CMIOExtensionProvider.startService(provider: providerSource.provider)
CFRunLoopRun()
