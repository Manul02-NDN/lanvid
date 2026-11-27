import AVFoundation
import SwiftUI
import UIKit

/// Live AVCaptureVideoPreviewLayer host with native UIKit multi-touch camera gestures:
/// - Single Tap: Focus & Meter Exposure at exact hardware sensor coordinate
/// - Double Tap: Instant Reset AF/AE & EV 0.0
/// - Long Press: AE/AF Lock at point
/// - Pinch: Smooth continuous optical + digital zoom (0.5x – 10x)
/// - 1-Finger Vertical Pan: Exposure compensation (-3.0 ... +3.0 EV)
/// - 1-Finger Horizontal Pan: Precision Manual Focus pull (0% ... 100%)
struct CameraLayerPreview: UIViewRepresentable {
    let session: AVCaptureSession
    var connectionPosition: AVCaptureDevice.Position? = nil
    var gestureCallbacks: ViewfinderGestureCallbacks? = nil

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        weak var boundSession: AVCaptureSession?
        var boundPosition: AVCaptureDevice.Position?
        weak var multiCamConnection: AVCaptureConnection?
        weak var containerView: PreviewContainerView?
        var callbacks: ViewfinderGestureCallbacks?

        // Gesture tracking state
        private var pinchStartZoom: CGFloat = 1.0
        private var panAxis: PanAxis = .undetermined
        private var panStartEv: Float = 0.0
        private var panStartFocus: Float = 0.5

        enum PanAxis { case undetermined, vertical, horizontal }

        @objc func handleTap(_ gr: UITapGestureRecognizer) {
            guard let v = containerView, v.bounds.width > 1, v.bounds.height > 1 else { return }
            let pt = gr.location(in: v)
            let norm = CGPoint(x: max(0.05, min(0.95, pt.x / v.bounds.width)),
                               y: max(0.08, min(0.92, pt.y / v.bounds.height)))
            let devPt = v.previewLayer.captureDevicePointConverted(fromLayerPoint: pt)
            callbacks?.onTapFocus(devPt, norm)
        }

        @objc func handleDoubleTap(_ gr: UITapGestureRecognizer) {
            callbacks?.onDoubleTapReset()
        }

        @objc func handleLongPress(_ gr: UILongPressGestureRecognizer) {
            guard gr.state == .began, let v = containerView, v.bounds.width > 1, v.bounds.height > 1 else { return }
            let pt = gr.location(in: v)
            let norm = CGPoint(x: max(0.05, min(0.95, pt.x / v.bounds.width)),
                               y: max(0.08, min(0.92, pt.y / v.bounds.height)))
            let devPt = v.previewLayer.captureDevicePointConverted(fromLayerPoint: pt)
            callbacks?.onLongPressLock(devPt, norm)
        }

        @objc func handlePinch(_ gr: UIPinchGestureRecognizer) {
            switch gr.state {
            case .began:
                pinchStartZoom = callbacks?.currentZoom() ?? 1.0
                callbacks?.onZoomActiveChanged(true)
            case .changed:
                callbacks?.onPinchZoom(pinchStartZoom * gr.scale)
            default:
                callbacks?.onZoomActiveChanged(false)
            }
        }

        @objc func handlePan(_ gr: UIPanGestureRecognizer) {
            guard let v = containerView, v.bounds.width > 1, v.bounds.height > 1 else { return }
            let tr = gr.translation(in: v)
            switch gr.state {
            case .began:
                panAxis = .undetermined
                panStartEv = callbacks?.currentExposureBias() ?? 0.0
                panStartFocus = callbacks?.currentFocusPosition() ?? 0.5
            case .changed:
                if panAxis == .undetermined {
                    if abs(tr.y) > 12 || abs(tr.x) > 12 {
                        panAxis = abs(tr.y) >= abs(tr.x) ? .vertical : .horizontal
                    } else {
                        return
                    }
                }
                switch panAxis {
                case .vertical:
                    // Swipe UP brightens (+EV), swipe DOWN darkens (-EV)
                    let deltaEv = Float(-tr.y / max(180.0, v.bounds.height * 0.42)) * 3.0
                    callbacks?.onExposureDrag(panStartEv + deltaEv, true)
                case .horizontal:
                    // Swipe RIGHT focuses farther, swipe LEFT focuses closer
                    let deltaFocus = Float(tr.x / max(180.0, v.bounds.width * 0.65))
                    callbacks?.onFocusDrag(panStartFocus + deltaFocus, true)
                case .undetermined:
                    break
                }
            default:
                if panAxis == .vertical {
                    let deltaEv = Float(-tr.y / max(180.0, v.bounds.height * 0.42)) * 3.0
                    callbacks?.onExposureDrag(panStartEv + deltaEv, false)
                } else if panAxis == .horizontal {
                    let deltaFocus = Float(tr.x / max(180.0, v.bounds.width * 0.65))
                    callbacks?.onFocusDrag(panStartFocus + deltaFocus, false)
                }
                panAxis = .undetermined
            }
        }

        func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
            return false
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> PreviewContainerView {
        let v = PreviewContainerView()
        v.previewLayer.videoGravity = .resizeAspectFill
        v.isMultipleTouchEnabled = true
        context.coordinator.containerView = v
        context.coordinator.callbacks = gestureCallbacks
        if gestureCallbacks != nil {
            installGestures(on: v, coordinator: context.coordinator)
        }
        attachSession(to: v, coordinator: context.coordinator)
        return v
    }

    func updateUIView(_ uiView: PreviewContainerView, context: Context) {
        context.coordinator.callbacks = gestureCallbacks
        if context.coordinator.boundSession !== session ||
            context.coordinator.boundPosition != connectionPosition ||
            (connectionPosition != nil && context.coordinator.multiCamConnection == nil) {
            attachSession(to: uiView, coordinator: context.coordinator)
        }
        let conn = context.coordinator.multiCamConnection ?? uiView.previewLayer.connection
        if let conn, conn.isVideoOrientationSupported {
            let interfaceOrient = uiView.window?.windowScene?.interfaceOrientation
            switch interfaceOrient {
            case .landscapeLeft: conn.videoOrientation = .landscapeLeft
            case .landscapeRight: conn.videoOrientation = .landscapeRight
            case .portraitUpsideDown: conn.videoOrientation = .portraitUpsideDown
            default: conn.videoOrientation = .portrait
            }
        }
    }

    static func dismantleUIView(_ uiView: PreviewContainerView, coordinator: Coordinator) {
        if let mc = coordinator.boundSession as? AVCaptureMultiCamSession,
           let conn = coordinator.multiCamConnection {
            mc.beginConfiguration()
            mc.removeConnection(conn)
            mc.commitConfiguration()
        }
        coordinator.multiCamConnection = nil
        coordinator.boundSession = nil
        uiView.previewLayer.session = nil
    }

    private func installGestures(on view: UIView, coordinator: Coordinator) {
        let doubleTap = UITapGestureRecognizer(target: coordinator, action: #selector(Coordinator.handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2

        let singleTap = UITapGestureRecognizer(target: coordinator, action: #selector(Coordinator.handleTap(_:)))
        singleTap.numberOfTapsRequired = 1
        singleTap.require(toFail: doubleTap)

        let longPress = UILongPressGestureRecognizer(target: coordinator, action: #selector(Coordinator.handleLongPress(_:)))
        longPress.minimumPressDuration = 0.45

        let pinch = UIPinchGestureRecognizer(target: coordinator, action: #selector(Coordinator.handlePinch(_:)))
        pinch.delegate = coordinator

        let pan = UIPanGestureRecognizer(target: coordinator, action: #selector(Coordinator.handlePan(_:)))
        pan.maximumNumberOfTouches = 1
        pan.delegate = coordinator
        pan.require(toFail: pinch)

        view.addGestureRecognizer(doubleTap)
        view.addGestureRecognizer(singleTap)
        view.addGestureRecognizer(longPress)
        view.addGestureRecognizer(pinch)
        view.addGestureRecognizer(pan)
    }

    private func attachSession(to uiView: PreviewContainerView, coordinator: Coordinator) {
        if let oldMc = coordinator.boundSession as? AVCaptureMultiCamSession,
           let oldConn = coordinator.multiCamConnection {
            oldMc.beginConfiguration()
            oldMc.removeConnection(oldConn)
            oldMc.commitConfiguration()
            coordinator.multiCamConnection = nil
        }

        if let pos = connectionPosition, let mc = session as? AVCaptureMultiCamSession {
            uiView.previewLayer.setSessionWithNoConnection(mc)
            if let input = mc.inputs.compactMap({ $0 as? AVCaptureDeviceInput })
                .first(where: { $0.device.position == pos }),
               let port = input.ports(for: .video, sourceDeviceType: input.device.deviceType, sourceDevicePosition: pos).first {
                let conn = AVCaptureConnection(inputPort: port, videoPreviewLayer: uiView.previewLayer)
                mc.beginConfiguration()
                if mc.canAddConnection(conn) {
                    mc.addConnection(conn)
                    coordinator.multiCamConnection = conn
                }
                mc.commitConfiguration()
            }
        } else {
            uiView.previewLayer.session = session
        }
        coordinator.boundSession = session
        coordinator.boundPosition = connectionPosition
    }

    final class PreviewContainerView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }
}

/// Callbacks bridging native UIKit multi-touch recognizers to CameraEngine.
struct ViewfinderGestureCallbacks {
    let currentZoom: () -> CGFloat
    let currentExposureBias: () -> Float
    let currentFocusPosition: () -> Float
    let onTapFocus: (CGPoint, CGPoint) -> Void
    let onDoubleTapReset: () -> Void
    let onLongPressLock: (CGPoint, CGPoint) -> Void
    let onPinchZoom: (CGFloat) -> Void
    let onZoomActiveChanged: (Bool) -> Void
    let onExposureDrag: (Float, Bool) -> Void
    let onFocusDrag: (Float, Bool) -> Void
}

/// Transparent UIKit touch gesture overlay for Demo / Simulator viewfinder so all gestures work identically without hardware camera.
private struct DemoTouchSurfaceView: UIViewRepresentable {
    let callbacks: ViewfinderGestureCallbacks

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        weak var view: UIView?
        var callbacks: ViewfinderGestureCallbacks
        private var pinchStartZoom: CGFloat = 1.0
        private var panAxis: Int = 0 // 0=none, 1=vertical, 2=horizontal
        private var panStartEv: Float = 0.0
        private var panStartFocus: Float = 0.5

        init(callbacks: ViewfinderGestureCallbacks) {
            self.callbacks = callbacks
        }

        private func normalized(_ pt: CGPoint, in v: UIView) -> CGPoint {
            CGPoint(x: max(0.05, min(0.95, pt.x / max(1, v.bounds.width))),
                    y: max(0.08, min(0.92, pt.y / max(1, v.bounds.height))))
        }

        @objc func handleTap(_ gr: UITapGestureRecognizer) {
            guard let v = view else { return }
            let norm = normalized(gr.location(in: v), in: v)
            callbacks.onTapFocus(norm, norm)
        }

        @objc func handleDoubleTap(_ gr: UITapGestureRecognizer) {
            callbacks.onDoubleTapReset()
        }

        @objc func handleLongPress(_ gr: UILongPressGestureRecognizer) {
            guard gr.state == .began, let v = view else { return }
            let norm = normalized(gr.location(in: v), in: v)
            callbacks.onLongPressLock(norm, norm)
        }

        @objc func handlePinch(_ gr: UIPinchGestureRecognizer) {
            switch gr.state {
            case .began:
                pinchStartZoom = callbacks.currentZoom()
                callbacks.onZoomActiveChanged(true)
            case .changed:
                callbacks.onPinchZoom(pinchStartZoom * gr.scale)
            default:
                callbacks.onZoomActiveChanged(false)
            }
        }

        @objc func handlePan(_ gr: UIPanGestureRecognizer) {
            guard let v = view else { return }
            let tr = gr.translation(in: v)
            switch gr.state {
            case .began:
                panAxis = 0
                panStartEv = callbacks.currentExposureBias()
                panStartFocus = callbacks.currentFocusPosition()
            case .changed:
                if panAxis == 0 {
                    if abs(tr.y) > 12 || abs(tr.x) > 12 {
                        panAxis = abs(tr.y) >= abs(tr.x) ? 1 : 2
                    } else { return }
                }
                if panAxis == 1 {
                    let deltaEv = Float(-tr.y / max(180.0, v.bounds.height * 0.42)) * 3.0
                    callbacks.onExposureDrag(panStartEv + deltaEv, true)
                } else {
                    let deltaFocus = Float(tr.x / max(180.0, v.bounds.width * 0.65))
                    callbacks.onFocusDrag(panStartFocus + deltaFocus, true)
                }
            default:
                if panAxis == 1 {
                    let deltaEv = Float(-tr.y / max(180.0, v.bounds.height * 0.42)) * 3.0
                    callbacks.onExposureDrag(panStartEv + deltaEv, false)
                } else if panAxis == 2 {
                    let deltaFocus = Float(tr.x / max(180.0, v.bounds.width * 0.65))
                    callbacks.onFocusDrag(panStartFocus + deltaFocus, false)
                }
                panAxis = 0
            }
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(callbacks: callbacks) }

    func makeUIView(context: Context) -> UIView {
        let v = UIView()
        v.backgroundColor = .clear
        v.isMultipleTouchEnabled = true
        context.coordinator.view = v

        let doubleTap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        let singleTap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handleTap(_:)))
        singleTap.require(toFail: doubleTap)
        let longPress = UILongPressGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handleLongPress(_:)))
        longPress.minimumPressDuration = 0.45
        let pinch = UIPinchGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handlePinch(_:)))
        let pan = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.handlePan(_:)))
        pan.maximumNumberOfTouches = 1
        pan.require(toFail: pinch)

        v.addGestureRecognizer(doubleTap)
        v.addGestureRecognizer(singleTap)
        v.addGestureRecognizer(longPress)
        v.addGestureRecognizer(pinch)
        v.addGestureRecognizer(pan)
        return v
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.callbacks = callbacks
    }
}

/// Renders either the real AVFoundation preview (Single / Dual-Cam PIP / 50-50 Split)
/// or the clean LanVid Studio neutral Demo viewfinder, plus interactive Focus/EV/MF touch overlays.
struct ViewfinderStageView: View {
    @ObservedObject var engine: CameraEngine
    @Environment(\.studio) private var s
    let isLandscape: Bool
    var onDismissDrawers: (() -> Void)? = nil

    private var callbacks: ViewfinderGestureCallbacks {
        ViewfinderGestureCallbacks(
            currentZoom: { engine.zoomFactor },
            currentExposureBias: { engine.exposureBias },
            currentFocusPosition: { engine.focusPosition },
            onTapFocus: { devPt, normPt in
                onDismissDrawers?()
                engine.focusAndExpose(atDevicePoint: devPt, viewNormalizedPoint: normPt)
            },
            onDoubleTapReset: {
                engine.resetTouchFocusAndExposure()
            },
            onLongPressLock: { devPt, normPt in
                engine.lockFocusAndExposure(atDevicePoint: devPt, viewNormalizedPoint: normPt)
            },
            onPinchZoom: { z in
                engine.setDisplayZoom(z, animated: false, showOverlay: true)
            },
            onZoomActiveChanged: { active in
                engine.touchHUD.adjustingZoom = active
            },
            onExposureDrag: { ev, active in
                engine.setExposureBias(ev, activeDrag: active)
            },
            onFocusDrag: { pos, active in
                engine.setTouchManualFocus(pos, activeDrag: active)
            }
        )
    }

    var body: some View {
        GeometryReader { geo in
            ZStack {
                if engine.isDemoMode || !engine.isPreviewActive {
                    demoStage
                    DemoTouchSurfaceView(callbacks: callbacks)
                } else if engine.duoMode == .dualCamSplit, let mc = engine.multiCamSession {
                    splitDualHardware(mc)
                } else if engine.duoMode == .dualCamPIP, let mc = engine.multiCamSession {
                    pipDualHardware(mc)
                } else {
                    CameraLayerPreview(session: engine.singleSession, gestureCallbacks: callbacks)
                }

                // Cinema rule-of-thirds & crosshair guides
                if engine.showGuides && engine.duoMode != .duoCompanion {
                    cinemaGuidesOverlay
                }

                // Subtle EIS Stabilization crop safe-margin guide when active
                if engine.stabilizationLevel != .off && engine.showGuides && engine.duoMode != .duoCompanion {
                    stabilizationMarginOverlay(cropPct: engine.stabilizationLevel.cropPercent)
                }

                // Focus Peaking overlay
                if engine.focusPeaking && engine.duoMode != .duoCompanion {
                    focusPeakingOverlay
                }

                // Overexposure Zebra overlay
                if engine.zebra && engine.duoMode != .duoCompanion {
                    zebraOverlay
                }

                // Animated Touch Focus & Exposure Reticle + Attached Vertical EV Slider
                if engine.touchHUD.visible && engine.duoMode != .duoCompanion {
                    touchReticleOverlay(in: geo.size)
                        .allowsHitTesting(false)
                }

                // Precision Horizontal Manual Focus Pull Ruler
                if engine.touchHUD.adjustingFocus && engine.duoMode != .duoCompanion {
                    manualFocusRulerOverlay
                        .allowsHitTesting(false)
                }
            }
        }
        .clipped()
    }

    // MARK: - Touch Reticle + Vertical Exposure (EV) Sun Slider

    private func touchReticleOverlay(in size: CGSize) -> some View {
        let px = engine.touchHUD.normalizedPoint.x * size.width
        let py = engine.touchHUD.normalizedPoint.y * size.height
        let reticleTint: Color = engine.touchHUD.locked ? s.amber : s.yellow
        let sliderOnLeft = px > size.width * 0.74
        let evNorm = CGFloat((engine.exposureBias + 3.0) / 6.0) // 0..1

        return ZStack {
            // Focus bracket box
            ZStack {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .stroke(reticleTint, lineWidth: engine.touchHUD.locked ? 2.0 : 1.5)
                    .frame(width: 68, height: 68)
                    .shadow(color: Color.black.opacity(0.55), radius: 4)

                // Center tick marks
                Path { p in
                    let r: CGFloat = 34
                    let t: CGFloat = 6
                    p.move(to: CGPoint(x: r, y: 0)); p.addLine(to: CGPoint(x: r, y: t))
                    p.move(to: CGPoint(x: r, y: 68 - t)); p.addLine(to: CGPoint(x: r, y: 68))
                    p.move(to: CGPoint(x: 0, y: r)); p.addLine(to: CGPoint(x: t, y: r))
                    p.move(to: CGPoint(x: 68 - t, y: r)); p.addLine(to: CGPoint(x: 68, y: r))
                }
                .stroke(reticleTint, lineWidth: 1.5)
                .frame(width: 68, height: 68)

                if engine.touchHUD.locked {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.black)
                        .padding(4)
                        .background(Circle().fill(s.amber))
                        .offset(y: -34)
                }
            }
            .position(x: px, y: py)

            // Attached Vertical Sun Exposure Bar
            VStack(spacing: 4) {
                Image(systemName: "sun.max.fill")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(reticleTint)

                ZStack(alignment: .bottom) {
                    Capsule()
                        .fill(Color.white.opacity(0.24))
                        .frame(width: 3, height: 84)
                    // Center zero tick
                    Rectangle()
                        .fill(Color.white.opacity(0.65))
                        .frame(width: 9, height: 1)
                        .offset(y: -42)
                    // Sun thumb position
                    Circle()
                        .fill(reticleTint)
                        .frame(width: 10, height: 10)
                        .shadow(color: .black.opacity(0.45), radius: 2)
                        .offset(y: -(evNorm * 74))
                }
                .frame(width: 14, height: 84)

                Text(String(format: "%+.1f", engine.exposureBias))
                    .font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                    .foregroundColor(reticleTint)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(Color.black.opacity(0.65)))
            }
            .position(x: px + (sliderOnLeft ? -54 : 54), y: py)
        }
        .transition(.opacity.combined(with: .scale(scale: 1.12)))
    }

    // MARK: - Horizontal Focus Pull Ruler HUD

    private var manualFocusRulerOverlay: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: "scope")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(s.accent)
                Text(tr("РУЧНИЙ ФОКУС", "MANUAL FOCUS"))
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundColor(s.text2)
                Text("\(Int(engine.focusPosition * 100))%")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(s.text)
            }
            ZStack(alignment: .leading) {
                // Ruler ticks
                HStack(spacing: 7) {
                    ForEach(0..<25, id: \.self) { i in
                        Rectangle()
                            .fill(i % 5 == 0 ? s.text2 : s.text3.opacity(0.55))
                            .frame(width: 1.5, height: i % 5 == 0 ? 14 : 8)
                    }
                }
                // A/B markers
                Circle()
                    .fill(s.amber)
                    .frame(width: 5, height: 5)
                    .offset(x: CGFloat(engine.focusPointA) * 204 - 2.5, y: 11)
                Circle()
                    .fill(s.green)
                    .frame(width: 5, height: 5)
                    .offset(x: CGFloat(engine.focusPointB) * 204 - 2.5, y: 11)
                // Active needle
                RoundedRectangle(cornerRadius: 2)
                    .fill(s.accent)
                    .frame(width: 3, height: 22)
                    .offset(x: CGFloat(engine.focusPosition) * 204 - 1.5)
            }
            .frame(width: 204, height: 26)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.hudSurface))
        .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.borderStrong, lineWidth: 1))
        .shadow(color: Color.black.opacity(0.35), radius: 12, y: 4)
        .transition(.opacity)
    }

    // MARK: - Dual Hardware Preview

    @ViewBuilder
    private func splitDualHardware(_ mc: AVCaptureMultiCamSession) -> some View {
        if isLandscape {
            HStack(spacing: 2) {
                CameraLayerPreview(session: mc, connectionPosition: .back, gestureCallbacks: callbacks)
                CameraLayerPreview(session: mc, connectionPosition: .front)
            }
        } else {
            VStack(spacing: 2) {
                CameraLayerPreview(session: mc, connectionPosition: .back, gestureCallbacks: callbacks)
                CameraLayerPreview(session: mc, connectionPosition: .front)
            }
        }
    }

    private func pipDualHardware(_ mc: AVCaptureMultiCamSession) -> some View {
        ZStack(alignment: .topTrailing) {
            CameraLayerPreview(session: mc, connectionPosition: .back, gestureCallbacks: callbacks)
            CameraLayerPreview(session: mc, connectionPosition: .front)
                .frame(width: 118, height: 158)
                .clipShape(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.borderStrong, lineWidth: 1.5))
                .shadow(color: .black.opacity(0.45), radius: 10, y: 4)
                .padding(.top, 96)
                .padding(.trailing, 16)
        }
    }

    // MARK: - Clean Neutral LanVid Studio Demo Viewfinder

    @ViewBuilder
    private var demoStage: some View {
        if engine.duoMode == .dualCamSplit {
            if isLandscape {
                HStack(spacing: 2) {
                    syntheticCinemaFeed(title: "MAIN · \(engine.lens.rawValue)", warmShift: 0.0, isSecondary: false)
                    syntheticCinemaFeed(title: "FRONT · MULTI-CAM", warmShift: 0.08, isSecondary: true)
                }
            } else {
                VStack(spacing: 2) {
                    syntheticCinemaFeed(title: "MAIN · \(engine.lens.rawValue)", warmShift: 0.0, isSecondary: false)
                    syntheticCinemaFeed(title: "FRONT · MULTI-CAM", warmShift: 0.08, isSecondary: true)
                }
            }
        } else if engine.duoMode == .dualCamPIP {
            ZStack(alignment: .topTrailing) {
                syntheticCinemaFeed(title: "MAIN · \(engine.lens.rawValue)", warmShift: 0.0, isSecondary: false)
                syntheticCinemaFeed(title: "FRONT PIP", warmShift: 0.08, isSecondary: true)
                    .frame(width: isLandscape ? 176 : 120, height: isLandscape ? 108 : 160)
                    .clipShape(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.borderStrong, lineWidth: 1.5))
                    .shadow(color: .black.opacity(0.5), radius: 10, y: 4)
                    .padding(.top, isLandscape ? 70 : 104)
                    .padding(.trailing, 14)
            }
        } else {
            syntheticCinemaFeed(title: "\(engine.displayBadgeTag) · \(String(format: "%.1f×", engine.zoomFactor))", warmShift: 0.0, isSecondary: false)
        }
    }

    private func syntheticCinemaFeed(title: String, warmShift: Double, isSecondary: Bool) -> some View {
        let wbNorm = Double((engine.wbKelvin - 5600.0) / 5000.0)
        let expBoost = isSecondary
            ? Double(engine.secondaryExposureBias) * 0.12
            : (Double(engine.exposureBias) * 0.12 + Double(engine.isoValue - 400.0) / 5500.0)
        let blurAmt = isSecondary ? 0.0 : abs(Double(engine.focusPosition) - 0.48) * 8.0
        let zoomScale = isSecondary ? 1.0 : max(0.75, min(2.6, 0.72 + Double(engine.zoomFactor) * 0.28))

        return GeometryReader { g in
            ZStack {
                // Neutral Studio dot-grid canvas (matches ref2 map-canvas aesthetic)
                StudioDotGridBackground()

                // Subtle neutral studio softbox lighting (zero garish purple!)
                RadialGradient(
                    colors: [
                        Color(
                            red: 0.26 + max(0, wbNorm * 0.10) + warmShift,
                            green: 0.27 + max(0, wbNorm * 0.04),
                            blue: 0.30 + max(0, -wbNorm * 0.12)
                        ).opacity(0.55),
                        Color.clear
                    ],
                    center: UnitPoint(x: 0.36, y: 0.36),
                    startRadius: 8,
                    endRadius: max(g.size.width, g.size.height) * 0.52
                )

                RadialGradient(
                    colors: [
                        s.accent.opacity(0.14),
                        Color.clear
                    ],
                    center: UnitPoint(x: 0.72, y: 0.62),
                    startRadius: 8,
                    endRadius: max(g.size.width, g.size.height) * 0.42
                )

                // Clean geometric cinema framing target (scales live with pinch zoom & blurs live with focus pull!)
                VStack(spacing: 14) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 20, style: .continuous)
                            .fill(Color.white.opacity(0.04))
                            .frame(width: 96, height: 96)
                            .overlay(
                                RoundedRectangle(cornerRadius: 20, style: .continuous)
                                    .stroke(Color.white.opacity(0.10), lineWidth: 1)
                            )
                        Image(systemName: isSecondary ? "person.crop.circle" : "video.fill")
                            .font(.system(size: 36, weight: .regular))
                            .foregroundColor(Color(hex: 0xECECEC).opacity(0.78))
                    }

                    HStack(spacing: 6) {
                        Circle()
                            .fill(engine.isStreaming ? s.green : s.text3)
                            .frame(width: 6, height: 6)
                        Text(title)
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .foregroundColor(Color(hex: 0xECECEC).opacity(0.88))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(Capsule().fill(Color(hex: 0x262626).opacity(0.85)))
                    .overlay(Capsule().stroke(Color.white.opacity(0.10), lineWidth: 0.75))

                    if !isSecondary {
                        HStack(spacing: 5) {
                            Text(tr("Тап — фокус", "Tap: Focus"))
                            Text("·")
                            Image(systemName: "arrow.up.and.down")
                                .font(.system(size: 9.5, weight: .semibold))
                            Text("EV")
                            Text("·")
                            Image(systemName: "arrow.left.and.right")
                                .font(.system(size: 9.5, weight: .semibold))
                            Text("MF")
                            Text("·")
                            Text(tr("Щипок — зум", "Pinch: Zoom"))
                        }
                        .font(.system(size: 10.5, weight: .medium))
                        .foregroundColor(Color(hex: 0xA8A8AD).opacity(0.75))
                    }
                }
                .scaleEffect(zoomScale)
                .blur(radius: blurAmt)
                .animation(.interactiveSpring(response: 0.20, dampingFraction: 0.85), value: zoomScale)
            }
            .brightness(max(-0.28, min(0.32, expBoost)))
        }
    }

    // MARK: - Viewfinder Assists

    private var cinemaGuidesOverlay: some View {
        GeometryReader { g in
            Path { p in
                let w = g.size.width, h = g.size.height
                p.move(to: CGPoint(x: w / 3, y: 0)); p.addLine(to: CGPoint(x: w / 3, y: h))
                p.move(to: CGPoint(x: 2 * w / 3, y: 0)); p.addLine(to: CGPoint(x: 2 * w / 3, y: h))
                p.move(to: CGPoint(x: 0, y: h / 3)); p.addLine(to: CGPoint(x: w, y: h / 3))
                p.move(to: CGPoint(x: 0, y: 2 * h / 3)); p.addLine(to: CGPoint(x: w, y: 2 * h / 3))
                let cx = w / 2, cy = h / 2
                p.move(to: CGPoint(x: cx - 12, y: cy)); p.addLine(to: CGPoint(x: cx + 12, y: cy))
                p.move(to: CGPoint(x: cx, y: cy - 12)); p.addLine(to: CGPoint(x: cx, y: cy + 12))
            }
            .stroke(Color.white.opacity(0.15), lineWidth: 0.75)
        }
        .allowsHitTesting(false)
    }

    private func stabilizationMarginOverlay(cropPct: Int) -> some View {
        let insetFrac = CGFloat(cropPct) / 260.0
        return GeometryReader { g in
            let dx = g.size.width * insetFrac
            let dy = g.size.height * insetFrac
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(s.accent.opacity(0.32), style: StrokeStyle(lineWidth: 1, dash: [6, 5]))
                .padding(.horizontal, dx)
                .padding(.vertical, dy)
        }
        .allowsHitTesting(false)
    }

    private var focusPeakingOverlay: some View {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
            .strokeBorder(s.green.opacity(0.60), style: StrokeStyle(lineWidth: 1.5, dash: [8, 6]))
            .padding(42)
            .allowsHitTesting(false)
    }

    private var zebraOverlay: some View {
        GeometryReader { g in
            Path { p in
                let step: CGFloat = 18
                var x: CGFloat = -g.size.height
                while x < g.size.width + g.size.height {
                    p.move(to: CGPoint(x: x, y: 0))
                    p.addLine(to: CGPoint(x: x + g.size.height * 0.22, y: g.size.height * 0.22))
                    x += step
                }
            }
            .stroke(Color.white.opacity(0.11), lineWidth: 1.5)
        }
        .allowsHitTesting(false)
    }
}
