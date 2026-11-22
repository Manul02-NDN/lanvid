// Example: iOS Camera (AVFoundation NV12) + LAN Discovery Sender using LanVidKit
import AVFoundation
import SwiftUI
import LanVidKit

final class CameraSessionController: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    @Published var isStreaming = false
    @Published var peers: [LanVidPeer] = []
    @Published var destIp = "192.168.1.100"
    @Published var statusText = "Ready (NEON: \(LanVidRuntime.simdInfo))"

    private let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "lanvid.ios.camera")
    private var sender: LanVidSender?

    func scanLan() {
        Task { @MainActor in
            peers = await LanVidDiscovery.scanOnce()
            if let first = peers.first { destIp = first.ip }
        }
    }

    func toggleStreaming() {
        if isStreaming {
            session.stopRunning()
            sender?.stop()
            isStreaming = false
            statusText = "Stopped"
            return
        }
        do {
            let s = try LanVidSender()
            try s.start(config: .init(destHost: destIp, destPort: 5000, quality: 80, fecGroupSize: 6, adaptiveQuality: true))
            sender = s
            configureCamera()
            queue.async { self.session.startRunning() }
            isStreaming = true
            statusText = "Streaming NV12 -> \(destIp):5000"
        } catch {
            statusText = "Error: \(error.localizedDescription)"
        }
    }

    private func configureCamera() {
        session.beginConfiguration()
        session.sessionPreset = .hd1280x720
        if session.inputs.isEmpty,
           let cam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
           let input = try? AVCaptureDeviceInput(device: cam) {
            session.addInput(input)
        }
        if session.outputs.isEmpty {
            let out = AVCaptureVideoDataOutput()
            out.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            ]
            out.alwaysDiscardsLateVideoFrames = true
            out.setSampleBufferDelegate(self, queue: queue)
            session.addOutput(out)
        }
        session.commitConfiguration()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        try? sender?.push(sampleBuffer: sampleBuffer)
    }
}

struct ContentView: View {
    @StateObject private var ctrl = CameraSessionController()

    var body: some View {
        VStack(spacing: 14) {
            Text("LanVid iOS Camera Sender").font(.title2.bold())
            TextField("Receiver IP", text: $ctrl.destIp).textFieldStyle(.roundedBorder)
            Button("Scan LAN") { ctrl.scanLan() }
            ForEach(ctrl.peers) { peer in
                Button("\(peer.name) (\(peer.ip):\(peer.port))") { ctrl.destIp = peer.ip }
            }
            Button(ctrl.isStreaming ? "Stop Streaming" : "Start Camera Stream") {
                ctrl.toggleStreaming()
            }
            .buttonStyle(.borderedProminent)
            Text(ctrl.statusText).font(.caption).foregroundColor(.secondary)
        }
        .padding()
        .onAppear { ctrl.scanLan() }
    }
}
