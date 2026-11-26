import AVFoundation
import SwiftUI

/// Live AVCaptureVideoPreviewLayer host for hardware camera feeds.
struct CameraLayerPreview: UIViewRepresentable {
    let session: AVCaptureSession
    var connectionPosition: AVCaptureDevice.Position? = nil

    func makeUIView(context: Context) -> PreviewContainerView {
        let v = PreviewContainerView()
        v.previewLayer.session = session
        v.previewLayer.videoGravity = .resizeAspectFill
        return v
    }

    func updateUIView(_ uiView: PreviewContainerView, context: Context) {
        if uiView.previewLayer.session !== session {
            uiView.previewLayer.session = session
        }
        if let conn = uiView.previewLayer.connection, conn.isVideoOrientationSupported {
            switch UIDevice.current.orientation {
            case .landscapeLeft: conn.videoOrientation = .landscapeRight
            case .landscapeRight: conn.videoOrientation = .landscapeLeft
            case .portraitUpsideDown: conn.videoOrientation = .portraitUpsideDown
            default: conn.videoOrientation = .portrait
            }
        }
    }

    final class PreviewContainerView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }
}

/// Renders either the real AVFoundation preview (Single / Dual-Cam PIP / 50-50 Split)
/// or the interactive Demo Cine Studio viewfinder (complete with focus blur, WB tint, Peaking, Zebra & Guides).
struct ViewfinderStageView: View {
    @ObservedObject var engine: CameraEngine
    let isLandscape: Bool

    var body: some View {
        ZStack {
            if engine.isDemoMode || !engine.isStreaming {
                demoStage
            } else if engine.duoMode == .dualCamSplit, let mc = engine.multiCamSession {
                splitDualHardware(mc)
            } else if engine.duoMode == .dualCamPIP, let mc = engine.multiCamSession {
                pipDualHardware(mc)
            } else {
                CameraLayerPreview(session: engine.singleSession)
            }

            if engine.showGuides && engine.duoMode != .duoCompanion {
                cinemaGuidesOverlay
            }
            if engine.focusPeaking && engine.duoMode != .duoCompanion {
                focusPeakingOverlay
            }
            if engine.zebra && engine.duoMode != .duoCompanion {
                zebraOverlay
            }
        }
        .clipped()
    }

    @ViewBuilder
    private func splitDualHardware(_ mc: AVCaptureMultiCamSession) -> some View {
        if isLandscape {
            HStack(spacing: 2) {
                CameraLayerPreview(session: mc, connectionPosition: .back)
                CameraLayerPreview(session: mc, connectionPosition: .front)
            }
        } else {
            VStack(spacing: 2) {
                CameraLayerPreview(session: mc, connectionPosition: .back)
                CameraLayerPreview(session: mc, connectionPosition: .front)
            }
        }
    }

    private func pipDualHardware(_ mc: AVCaptureMultiCamSession) -> some View {
        ZStack(alignment: .topTrailing) {
            CameraLayerPreview(session: mc, connectionPosition: .back)
            CameraLayerPreview(session: mc, connectionPosition: .front)
                .frame(width: 118, height: 158)
                .clipShape(RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14).stroke(CineTheme.lavenderBright, lineWidth: 2))
                .padding(.top, 92)
                .padding(.trailing, 16)
        }
    }

    @ViewBuilder
    private var demoStage: some View {
        if engine.duoMode == .dualCamSplit {
            if isLandscape {
                HStack(spacing: 3) {
                    syntheticCinemaFeed(title: "MAIN • \(engine.lens.rawValue) BACK", warmShift: 0.0, isSecondary: false)
                    syntheticCinemaFeed(title: "FRONT • MULTI-CAM", warmShift: 0.14, isSecondary: true)
                }
            } else {
                VStack(spacing: 3) {
                    syntheticCinemaFeed(title: "MAIN • \(engine.lens.rawValue) BACK", warmShift: 0.0, isSecondary: false)
                    syntheticCinemaFeed(title: "FRONT • MULTI-CAM", warmShift: 0.14, isSecondary: true)
                }
            }
        } else if engine.duoMode == .dualCamPIP {
            ZStack(alignment: .topTrailing) {
                syntheticCinemaFeed(title: "MAIN • \(engine.lens.rawValue) BACK", warmShift: 0.0, isSecondary: false)
                syntheticCinemaFeed(title: "FRONT PIP", warmShift: 0.14, isSecondary: true)
                    .frame(width: isLandscape ? 180 : 124, height: isLandscape ? 112 : 166)
                    .clipShape(RoundedRectangle(cornerRadius: 14))
                    .overlay(RoundedRectangle(cornerRadius: 14).stroke(CineTheme.lavenderBright, lineWidth: 2))
                    .shadow(color: .black.opacity(0.55), radius: 10)
                    .padding(.top, isLandscape ? 72 : 108)
                    .padding(.trailing, 16)
            }
        } else {
            syntheticCinemaFeed(title: "\(engine.cameraLabel) • \(engine.lens.displayLabel)", warmShift: 0.0, isSecondary: false)
        }
    }

    private func syntheticCinemaFeed(title: String, warmShift: Double, isSecondary: Bool) -> some View {
        let wbNorm = Double((engine.wbKelvin - 5600.0) / 4000.0)
        let expBoost = isSecondary
            ? Double(engine.secondaryExposureBias) * 0.12
            : (Double(engine.isoValue - 400.0) / 4000.0)
        let blurAmt = isSecondary ? 0.0 : abs(Double(engine.focusPosition) - 0.48) * 9.0

        return GeometryReader { g in
            ZStack {
                LinearGradient(
                    colors: [
                        Color(red: 0.09 + max(0, wbNorm * 0.08) + warmShift * 0.2,
                              green: 0.08 + max(0, expBoost * 0.15),
                              blue: 0.18 + max(0, -wbNorm * 0.1)),
                        Color(red: 0.18 + max(0, wbNorm * 0.10),
                              green: 0.13 + max(0, expBoost * 0.18),
                              blue: 0.30 + max(0, -wbNorm * 0.12)),
                        Color(red: 0.06, green: 0.05, blue: 0.10)
                    ],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )

                // Studio key light bokeh circles
                Circle()
                    .fill(RadialGradient(
                        colors: [CineTheme.lavenderBright.opacity(0.55), Color.clear],
                        center: .center, startRadius: 4, endRadius: g.size.width * 0.36
                    ))
                    .frame(width: g.size.width * 0.65, height: g.size.width * 0.65)
                    .offset(x: -g.size.width * 0.18, y: -g.size.height * 0.14)

                Circle()
                    .fill(RadialGradient(
                        colors: [Color(red: 0.96, green: 0.52, blue: 0.44).opacity(0.38), Color.clear],
                        center: .center, startRadius: 4, endRadius: g.size.width * 0.32
                    ))
                    .frame(width: g.size.width * 0.52, height: g.size.width * 0.52)
                    .offset(x: g.size.width * 0.24, y: g.size.height * 0.08)

                // Stylized cinema subject silhouette (studio microphone / camera rig horizon)
                VStack(spacing: 12) {
                    Image(systemName: isSecondary ? "person.crop.circle.fill" : "video.badge.waveform.fill")
                        .font(.system(size: min(g.size.width, g.size.height) * 0.18, weight: .ultraLight))
                        .foregroundStyle(
                            LinearGradient(colors: [CineTheme.lavenderSoft.opacity(0.85), CineTheme.lavender.opacity(0.45)],
                                           startPoint: .top, endPoint: .bottom)
                        )
                    Text(title)
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(CineTheme.lavenderSoft.opacity(0.9))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color.black.opacity(0.45))
                        .clipShape(Capsule())
                }
                .blur(radius: blurAmt)
            }
            .brightness(max(-0.25, min(0.28, expBoost)))
        }
    }

    private var cinemaGuidesOverlay: some View {
        GeometryReader { g in
            Path { p in
                // Rule of thirds
                let w = g.size.width, h = g.size.height
                p.move(to: CGPoint(x: w / 3, y: 0)); p.addLine(to: CGPoint(x: w / 3, y: h))
                p.move(to: CGPoint(x: 2 * w / 3, y: 0)); p.addLine(to: CGPoint(x: 2 * w / 3, y: h))
                p.move(to: CGPoint(x: 0, y: h / 3)); p.addLine(to: CGPoint(x: w, y: h / 3))
                p.move(to: CGPoint(x: 0, y: 2 * h / 3)); p.addLine(to: CGPoint(x: w, y: 2 * h / 3))
                // Center crosshair
                let cx = w / 2, cy = h / 2
                p.move(to: CGPoint(x: cx - 14, y: cy)); p.addLine(to: CGPoint(x: cx + 14, y: cy))
                p.move(to: CGPoint(x: cx, y: cy - 14)); p.addLine(to: CGPoint(x: cx, y: cy + 14))
            }
            .stroke(Color.white.opacity(0.18), lineWidth: 0.75)
        }
        .allowsHitTesting(false)
    }

    private var focusPeakingOverlay: some View {
        RoundedRectangle(cornerRadius: 20)
            .strokeBorder(CineTheme.lavenderBright.opacity(0.55), style: StrokeStyle(lineWidth: 1.5, dash: [8, 6]))
            .padding(44)
            .allowsHitTesting(false)
    }

    private var zebraOverlay: some View {
        GeometryReader { g in
            Path { p in
                let step: CGFloat = 18
                var x: CGFloat = -g.size.height
                while x < g.size.width + g.size.height {
                    p.move(to: CGPoint(x: x, y: 0))
                    p.addLine(to: CGPoint(x: x + g.size.height * 0.25, y: g.size.height * 0.25))
                    x += step
                }
            }
            .stroke(Color.white.opacity(0.12), lineWidth: 2)
        }
        .allowsHitTesting(false)
    }
}
