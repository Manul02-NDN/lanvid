// Example: macOS Metal Receiver (MTKView + AVAudioEngine + Live HUD) using LanVidKit
import MetalKit
import SwiftUI
import LanVidKit

#if os(macOS)
struct MetalCanvasRepresentable: NSViewRepresentable {
    let receiver: LanVidReceiver

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        let renderer = LanVidMetalRenderer(metalView: view)
        context.coordinator.renderer = renderer
        receiver.onVideoPixelBuffer = { pb, _ in
            renderer?.enqueue(pb, in: view)
        }
        return view
    }

    func updateNSView(_ nsView: MTKView, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator() }
    final class Coordinator { var renderer: LanVidMetalRenderer? }
}

struct MacReceiverContentView: View {
    @State private var receiver: LanVidReceiver?
    @State private var running = false
    @State private var volume: Float = 1.0
    @State private var hudText = "Waiting for stream on UDP :5000"

    var body: some View {
        VStack(spacing: 10) {
            HStack {
                Text("LanVid macOS Metal Receiver").font(.headline)
                Spacer()
                Slider(value: $volume, in: 0...1.5).frame(width: 140)
                    .onChange(of: volume) { v in receiver?.setVolume(v) }
                Button(running ? "Stop" : "Start Receiver") { toggle() }
            }
            if let rx = receiver {
                MetalCanvasRepresentable(receiver: rx).frame(minHeight: 420).cornerRadius(8)
            }
            Text(hudText).font(.system(.caption, design: .monospaced))
        }
        .padding()
        .task {
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 500_000_000)
                if let s = receiver?.stats, running {
                    hudText = String(format: "%dx%d | %.1f FPS | E2E %.2f ms | %.1f Mbps | FEC:%llu | Aud %.0f kbps",
                                     s.width, s.height, s.fps, s.latencyP50Ms, s.bitrateMbps, s.fecRecovered, s.audioKbps)
                }
            }
        }
    }

    private func toggle() {
        if running {
            receiver?.stop()
            running = false
            return
        }
        do {
            let rx = try LanVidReceiver()
            try rx.start(config: .init(bindPort: 5000, enableAudioPlayback: true, enableDiscovery: true))
            receiver = rx
            running = true
        } catch {
            hudText = "Error: \(error.localizedDescription)"
        }
    }
}
#endif
