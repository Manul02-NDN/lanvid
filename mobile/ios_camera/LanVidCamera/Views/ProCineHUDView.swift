import SwiftUI

/// Adaptive Pro Cine HUD with Dynamic Island / Notch safe-area awareness (Portrait, Landscape Left/Right),
/// Compact / Pro Max scaling, Dual-Cam PIP/Split exposure controls, Duo Companion remote desk, and Demo Cine Studio.
struct ProCineHUDView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.horizontalSizeClass) private var hSizeClass
    @Environment(\.verticalSizeClass) private var vSizeClass

    @State var showManualDeck = false
    @State var showEncoderDrawer = false
    @State var showDuoSheet = false
    @State var showSettingsSheet = false
    @State private var showBadgeTooltip = false

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height
            let isCompactWidth = geo.size.width < 395 && !isLandscape
            let safeTop = max(geo.safeAreaInsets.top, isLandscape ? 10 : 12)
            let safeBot = max(geo.safeAreaInsets.bottom, isLandscape ? 10 : 12)
            let safeLead = max(geo.safeAreaInsets.leading, isLandscape ? 18 : 12)
            let safeTrail = max(geo.safeAreaInsets.trailing, isLandscape ? 18 : 12)

            ZStack {
                CineTheme.backdropGradient.ignoresSafeArea()

                if engine.duoMode == .duoCompanion {
                    duoCompanionDesk(isLandscape: isLandscape, safeTop: safeTop, safeBot: safeBot,
                                     safeLead: safeLead, safeTrail: safeTrail)
                } else {
                    ViewfinderStageView(engine: engine, isLandscape: isLandscape)
                        .ignoresSafeArea()

                    // Full-frame Tally border (Red PGM ON AIR / Green PVW PREVIEW)
                    if engine.tallyState != .standby {
                        RoundedRectangle(cornerRadius: 28, style: .continuous)
                            .stroke(engine.tallyState.accentColor, lineWidth: engine.tallyState == .program ? 8 : 5)
                            .ignoresSafeArea()
                            .allowsHitTesting(false)
                    }

                    VStack(spacing: isCompactWidth ? 6 : 8) {
                        topTelemetryBar(isLandscape: isLandscape, isCompactWidth: isCompactWidth)

                        HStack {
                            Spacer()
                            cameraNumberBadge
                        }

                        if engine.duoMode.isMultiCam || !engine.browser.duoPeers.isEmpty {
                            duoSyncBanner(isCompactWidth: isCompactWidth)
                        }

                        if !engine.isStreaming && !engine.isDemoMode {
                            Spacer(minLength: 4)
                            startConnectCard(isCompactWidth: isCompactWidth)
                            Spacer(minLength: 4)
                        } else {
                            Spacer(minLength: 4)

                            if engine.showHistogram && !showManualDeck && !showEncoderDrawer {
                                HStack {
                                    Spacer()
                                    liveMetersCluster(isCompactWidth: isCompactWidth)
                                }
                            }

                            if showManualDeck {
                                manualDeckPanel(isCompactWidth: isCompactWidth)
                            } else if showEncoderDrawer {
                                encoderDrawerPanel(isCompactWidth: isCompactWidth)
                            }
                        }

                        bottomControlBar(isLandscape: isLandscape, isCompactWidth: isCompactWidth)
                    }
                    .padding(.top, safeTop)
                    .padding(.bottom, safeBot)
                    .padding(.leading, safeLead)
                    .padding(.trailing, safeTrail)
                }
            }
            .ignoresSafeArea()
        }
        .sheet(isPresented: $showDuoSheet) {
            DuoSyncSheetView().environmentObject(engine)
        }
        .sheet(isPresented: $showSettingsSheet) {
            CameraSettingsView().environmentObject(engine)
        }
        .onAppear {
            applyLaunchScreenshotPresetIfNeeded()
        }
    }

    // MARK: - Top Telemetry & Tally Bar (Dynamic Island / Notch Safe)

    @ViewBuilder
    private func topTelemetryBar(isLandscape: Bool, isCompactWidth: Bool) -> some View {
        if isLandscape {
            HStack(spacing: 8) {
                tallyPillButton
                recBadgeIfActive
                directorBadgeIfActive
                Spacer(minLength: 4)
                telemetryReadout(compact: false)
                duoIconButton
                settingsIconButton
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(CineTheme.glass)
            .overlay(RoundedRectangle(cornerRadius: 16).stroke(CineTheme.border, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 16))
        } else {
            VStack(spacing: 6) {
                HStack(spacing: 6) {
                    tallyPillButton
                    recBadgeIfActive
                    Spacer(minLength: 4)
                    duoIconButton
                    settingsIconButton
                }
                HStack(spacing: 8) {
                    Text(engine.telemetry.timecode)
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(CineTheme.lavenderSoft)
                        .fixedSize()
                    Text(String(format: "%@ %dp · %.1f Mbps · %.0f ms",
                                engine.resolution.rawValue, engine.fps,
                                engine.telemetry.bitrateMbps, engine.telemetry.latencyMs))
                        .font(.system(size: isCompactWidth ? 9 : 10, weight: .medium, design: .monospaced))
                        .foregroundColor(CineTheme.textMuted)
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    directorBadgeIfActive
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(CineTheme.glass)
            .overlay(RoundedRectangle(cornerRadius: 16).stroke(CineTheme.border, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 16))
        }
    }

    private var cameraNumberBadge: some View {
        let borderTint: Color = {
            switch engine.tallyState {
            case .program: return CineTheme.tallyRed
            case .preview: return CineTheme.tallyGreen
            case .standby: return CineTheme.lavender
            }
        }()
        return VStack(alignment: .trailing, spacing: 5) {
            Button {
                withAnimation(.easeInOut(duration: 0.16)) { showBadgeTooltip.toggle() }
            } label: {
                HStack(spacing: 6) {
                    Circle()
                        .fill(borderTint)
                        .frame(width: 7, height: 7)
                    Text(engine.displayBadgeTag)
                        .font(.system(size: 12, weight: .heavy, design: .monospaced))
                        .foregroundColor(.white)
                        .lineLimit(1)
                        .fixedSize()
                }
                .padding(.horizontal, 11)
                .frame(minHeight: 36)
                .background(CineTheme.glass)
                .overlay(RoundedRectangle(cornerRadius: 11, style: .continuous).stroke(borderTint, lineWidth: 2))
                .clipShape(RoundedRectangle(cornerRadius: 11, style: .continuous))
                .shadow(color: borderTint.opacity(0.45), radius: 6, x: 0, y: 2)
            }

            if showBadgeTooltip {
                HStack(spacing: 8) {
                    let modeStr = engine.syncedFixedDelay ? "\(engine.syncedDelayMs) ms" : "Adaptive"
                    Text(engine.configSynced
                         ? tr("Призначено студією: \(engine.displayBadgeTag) • Synced \(modeStr)",
                              "Assigned by Studio: \(engine.displayBadgeTag) • Synced \(modeStr)")
                         : tr("Камера: \(engine.displayBadgeTag) • Очікування студії",
                              "Camera: \(engine.displayBadgeTag) • Awaiting Studio"))
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundColor(CineTheme.lavenderSoft)
                    Button(tr("Мережа", "Network")) {
                        showBadgeTooltip = false
                        showSettingsSheet = true
                    }
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(CineTheme.lavender.opacity(0.85))
                    .clipShape(Capsule())
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(CineTheme.glass)
                .overlay(RoundedRectangle(cornerRadius: 11).stroke(borderTint.opacity(0.7), lineWidth: 1))
                .clipShape(RoundedRectangle(cornerRadius: 11))
            }
        }
    }

    private var tallyPillButton: some View {
        Button {
            if engine.isDemoMode { engine.cycleDemoTally() }
        } label: {
            HStack(spacing: 5) {
                Circle()
                    .fill(Color.white)
                    .frame(width: 6, height: 6)
                    .opacity(engine.tallyState == .program ? 1 : 0.7)
                Text("\(engine.cameraLabel) · \(engine.tallyState.badgeText)")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .lineLimit(1)
                    .fixedSize()
            }
            .foregroundColor(.white)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(engine.tallyState.accentColor)
            .clipShape(Capsule())
        }
    }

    @ViewBuilder
    private var recBadgeIfActive: some View {
        if engine.remoteRec {
            Text("● REC")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(CineTheme.tallyRed)
                .clipShape(Capsule())
        }
    }

    @ViewBuilder
    private var directorBadgeIfActive: some View {
        if engine.directorSpeaking {
            Text(tr("🎙 РЕЖИСЕР", "🎙 DIRECTOR"))
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(CineTheme.lavender)
                .clipShape(Capsule())
        }
    }

    private func telemetryReadout(compact: Bool) -> some View {
        VStack(alignment: .trailing, spacing: 1) {
            Text(engine.telemetry.timecode)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(CineTheme.lavenderSoft)
            Text(String(format: "%@ %dp · %.1f Mbps · %.0f ms",
                        engine.resolution.rawValue, engine.fps,
                        engine.telemetry.bitrateMbps, engine.telemetry.latencyMs))
                .font(.system(size: compact ? 8 : 9, weight: .medium, design: .monospaced))
                .foregroundColor(CineTheme.textMuted)
        }
    }

    private var duoIconButton: some View {
        Button { showDuoSheet = true } label: {
            Image(systemName: "rectangle.split.2x1.fill")
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(engine.duoMode == .singleHUD ? CineTheme.lavenderSoft : .white)
                .padding(7)
                .background(engine.duoMode == .singleHUD ? CineTheme.glassLight : CineTheme.lavender)
                .clipShape(Circle())
        }
    }

    private var settingsIconButton: some View {
        Button { showSettingsSheet = true } label: {
            Image(systemName: "gearshape.fill")
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(CineTheme.lavenderSoft)
                .padding(7)
                .background(CineTheme.glassLight)
                .clipShape(Circle())
        }
    }

    // MARK: - iPhone Duo Sync Banner

    private func duoSyncBanner(isCompactWidth: Bool) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "iphone.radiowaves.left.and.right")
                .font(.system(size: 12, weight: .semibold))
                .foregroundColor(CineTheme.lavenderBright)

            Text(engine.duoMode.title)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(CineTheme.textPrimary)

            if let peer = engine.browser.duoPeers.first {
                Text("• \(peer.role.rawValue): \(peer.tally.badgeText)")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundColor(peer.tally.accentColor)
                    .lineLimit(1)
            }

            Spacer(minLength: 4)

            if engine.duoMode.isMultiCam && !isCompactWidth {
                Text(String(format: "FRONT EV %+.1f", engine.secondaryExposureBias))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(CineTheme.lavenderSoft)
            }

            Button(tr("Змінити", "Switch")) { showDuoSheet = true }
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.white)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(CineTheme.lavender.opacity(0.85))
                .clipShape(Capsule())
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(CineTheme.glass)
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(CineTheme.lavender.opacity(0.45), lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    // MARK: - Start / Connect Card + Prominent Demo Cine Studio Button

    private func startConnectCard(isCompactWidth: Bool) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("LanVid Camera")
                        .font(.system(size: isCompactWidth ? 18 : 20, weight: .bold))
                        .foregroundColor(CineTheme.textPrimary)
                    Text(tr("Pro Cine • Tally • Duplex Intercom • iPhone Duo",
                            "Pro Cine • Tally • Duplex Intercom • iPhone Duo"))
                        .font(.system(size: 11))
                        .foregroundColor(CineTheme.textMuted)
                }
                Spacer()
                Text("v2.7.0")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(CineTheme.lavenderSoft)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(CineTheme.glassLight)
                    .clipShape(Capsule())
            }

            HStack(spacing: 8) {
                TextField(tr("IP студії (LanVid Video)", "Studio IP (LanVid Video)"), text: $engine.destIp)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 13, design: .monospaced))
                    .keyboardType(.decimalPad)

                Button(tr("Пошук", "Scan")) { engine.startDiscovery() }
                    .buttonStyle(.bordered)
                    .tint(CineTheme.lavenderBright)
            }

            if !engine.browser.studios.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(engine.browser.studios) { st in
                            Button {
                                engine.selectStudio(st)
                            } label: {
                                HStack(spacing: 5) {
                                    Circle().fill(CineTheme.tallyGreen).frame(width: 7, height: 7)
                                    Text("\(st.name) (\(st.host))")
                                        .font(.system(size: 11, weight: .semibold))
                                        .foregroundColor(CineTheme.textPrimary)
                                }
                                .padding(.horizontal, 10)
                                .padding(.vertical, 6)
                                .background(CineTheme.glassLight)
                                .clipShape(Capsule())
                            }
                        }
                    }
                }
            }

            HStack(spacing: 8) {
                Button {
                    engine.startLiveStreaming()
                } label: {
                    Label(tr("В ЕФІР (GO LIVE)", "GO LIVE"), systemImage: "dot.radiowaves.left.and.right")
                        .font(.system(size: 13, weight: .bold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
                .tint(CineTheme.lavender)

                Button {
                    engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                    showManualDeck = true
                } label: {
                    Label(tr("Open Demo Cine Studio", "Open Demo Cine Studio"), systemImage: "sparkles.tv.fill")
                        .font(.system(size: 12, weight: .bold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.bordered)
                .tint(CineTheme.lavenderSoft)
            }

            Text(engine.statusText)
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(CineTheme.textMuted)
                .lineLimit(1)
        }
        .padding(14)
        .background(CineTheme.glass)
        .overlay(RoundedRectangle(cornerRadius: 20).stroke(CineTheme.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 20))
        .frame(maxWidth: 520)
    }

    // MARK: - Live Histogram & Stereo VU Meters

    private func liveMetersCluster(isCompactWidth: Bool) -> some View {
        VStack(alignment: .trailing, spacing: 6) {
            // Compact live RGB/Luma Histogram
            HStack(alignment: .bottom, spacing: 2) {
                ForEach(0..<engine.telemetry.histogram.count, id: \.self) { i in
                    RoundedRectangle(cornerRadius: 1)
                        .fill(CineTheme.lavenderSoft.opacity(0.85))
                        .frame(width: isCompactWidth ? 3 : 4,
                               height: max(3, CGFloat(engine.telemetry.histogram[i]) * 34))
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(CineTheme.glass)
            .clipShape(RoundedRectangle(cornerRadius: 10))

            // Dual Intercom / Mic VU bar
            HStack(spacing: 6) {
                vuPill(label: "MIC", dbfs: engine.telemetry.micPeakDbfs, tint: CineTheme.tallyRed)
                vuPill(label: "IFB", dbfs: engine.telemetry.ifbPeakDbfs, tint: CineTheme.tallyGreen)
            }
        }
    }

    private func vuPill(label: String, dbfs: Float, tint: Color) -> some View {
        let frac = max(0.05, min(1.0, CGFloat((dbfs + 60.0) / 60.0)))
        return HStack(spacing: 4) {
            Text(label)
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .foregroundColor(CineTheme.textMuted)
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.12)).frame(width: 48, height: 6)
                Capsule().fill(tint).frame(width: 48 * frac, height: 6)
            }
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 4)
        .background(CineTheme.glass)
        .clipShape(Capsule())
    }

    // MARK: - Expandable Pro Manual Camera Deck

    private func manualDeckPanel(isCompactWidth: Bool) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack {
                Text(tr("РУЧНА КІНОПАНЕЛЬ (PRO CINE DECK)", "PRO MANUAL CAMERA DECK"))
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(CineTheme.lavenderSoft)
                Spacer()
                if engine.isDemoMode {
                    Button(tr("Тест Tally", "Cycle Tally")) { engine.cycleDemoTally() }
                        .font(.system(size: 10, weight: .bold))
                        .buttonStyle(.bordered)
                        .tint(engine.tallyState.accentColor)
                }
            }

            // Lens selector + Viewfinder assists
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(CineLens.allCases) { l in
                        Button(l.displayLabel) { engine.selectLens(l) }
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .buttonStyle(.bordered)
                            .tint(engine.lens == l ? CineTheme.lavender : .gray)
                    }
                    Divider().frame(height: 18)
                    Toggle(tr("Пікінг", "Peaking"), isOn: $engine.focusPeaking).toggleStyle(.button).tint(CineTheme.lavender)
                    Toggle(tr("Зебра", "Zebra"), isOn: $engine.zebra).toggleStyle(.button).tint(CineTheme.lavender)
                    Toggle(tr("Сітка", "Guides"), isOn: $engine.showGuides).toggleStyle(.button).tint(CineTheme.lavender)
                }
                .font(.system(size: 10, design: .monospaced))
            }

            // Shutter angle
            HStack(spacing: 6) {
                Text("SHUTTER \(engine.shutterAngle)°")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(CineTheme.textPrimary)
                    .frame(width: isCompactWidth ? 76 : 88, alignment: .leading)
                ForEach([45, 90, 180, 360], id: \.self) { ang in
                    Button("\(ang)°") {
                        engine.autoExposure = false
                        engine.shutterAngle = ang
                        engine.applyManualCameraControls()
                    }
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .buttonStyle(.bordered)
                    .tint(engine.shutterAngle == ang ? CineTheme.lavender : .gray)
                }
            }

            // ISO slider
            HStack(spacing: 6) {
                Text("ISO \(Int(engine.isoValue))")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .frame(width: isCompactWidth ? 64 : 74, alignment: .leading)
                Slider(value: $engine.isoValue, in: 50...3200, step: 50)
                    .onChange(of: engine.isoValue) { _ in
                        engine.autoExposure = false
                        engine.applyManualCameraControls()
                    }
                    .tint(CineTheme.lavenderBright)
                Button(engine.autoExposure ? "AE" : "M") {
                    engine.autoExposure.toggle()
                    engine.applyManualCameraControls()
                }
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .buttonStyle(.bordered)
                .tint(engine.autoExposure ? .gray : CineTheme.lavender)
            }

            // White Balance Kelvin
            HStack(spacing: 6) {
                Text("WB \(Int(engine.wbKelvin))K")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .frame(width: isCompactWidth ? 64 : 74, alignment: .leading)
                Slider(value: $engine.wbKelvin, in: 2500...9500, step: 100)
                    .onChange(of: engine.wbKelvin) { _ in
                        engine.autoWhiteBalance = false
                        engine.applyManualCameraControls()
                    }
                    .tint(CineTheme.lavenderBright)
                Button(engine.autoWhiteBalance ? "AWB" : "M") {
                    engine.autoWhiteBalance.toggle()
                    engine.applyManualCameraControls()
                }
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .buttonStyle(.bordered)
                .tint(engine.autoWhiteBalance ? .gray : CineTheme.lavender)
            }

            // Manual Focus + Rack Focus A <-> B
            HStack(spacing: 6) {
                Text("MF \(Int(engine.focusPosition * 100))%")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .frame(width: isCompactWidth ? 64 : 74, alignment: .leading)
                Slider(value: $engine.focusPosition, in: 0...1)
                    .onChange(of: engine.focusPosition) { _ in
                        engine.autoFocus = false
                        engine.applyManualCameraControls()
                    }
                    .tint(CineTheme.lavenderBright)
                Button("A") { engine.rackFocus(to: engine.focusPointA) }
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .buttonStyle(.bordered)
                    .tint(CineTheme.lavenderSoft)
                Button("B") { engine.rackFocus(to: engine.focusPointB) }
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .buttonStyle(.bordered)
                    .tint(CineTheme.lavenderSoft)
            }

            if engine.duoMode.isMultiCam {
                HStack(spacing: 6) {
                    Text(String(format: "FRONT EV %+.1f", engine.secondaryExposureBias))
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(CineTheme.lavenderSoft)
                        .frame(width: 96, alignment: .leading)
                    Slider(value: $engine.secondaryExposureBias, in: -2.0...2.0, step: 0.2)
                        .onChange(of: engine.secondaryExposureBias) { _ in
                            engine.applyManualCameraControls()
                        }
                        .tint(CineTheme.tallyGreen)
                }
            }
        }
        .padding(12)
        .background(CineTheme.glass)
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(CineTheme.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    // MARK: - Expandable Hardware Encoder & Intercom Drawer

    private func encoderDrawerPanel(isCompactWidth: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(tr("АПАРАТНИЙ КОДЕР І ПОТІК", "HARDWARE ENCODER & STREAM"))
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(CineTheme.lavenderSoft)
                Spacer()
                Text(engine.telemetry.codecBackend)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(CineTheme.tallyGreen)
            }

            HStack(spacing: 6) {
                ForEach(CineResolution.allCases) { r in
                    Button(r.rawValue) { engine.resolution = r }
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .buttonStyle(.bordered)
                        .tint(engine.resolution == r ? CineTheme.lavender : .gray)
                }
                Spacer(minLength: 4)
                ForEach([24, 30, 60], id: \.self) { f in
                    Button("\(f)p") { engine.fps = f }
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .buttonStyle(.bordered)
                        .tint(engine.fps == f ? CineTheme.lavender : .gray)
                }
            }

            HStack(spacing: 6) {
                ForEach(CineCodec.allCases) { c in
                    Button(isCompactWidth ? c.shortName : c.rawValue) { engine.codec = c }
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .buttonStyle(.bordered)
                        .tint(engine.codec == c ? CineTheme.lavender : .gray)
                }
            }

            HStack(spacing: 8) {
                Text(String(format: "%@ %d Mbps", tr("Бітрейт", "Bitrate"), Int(engine.bitrateMbps)))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .frame(width: 112, alignment: .leading)
                Slider(value: $engine.bitrateMbps, in: 5...100, step: 5)
                    .tint(CineTheme.lavenderBright)
            }

            HStack(spacing: 8) {
                Text(tr("Гучність IFB", "IFB Volume"))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .frame(width: 112, alignment: .leading)
                Slider(value: $engine.ifbVolume, in: 0...1.5)
                    .tint(CineTheme.tallyGreen)
            }
        }
        .padding(12)
        .background(CineTheme.glass)
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(CineTheme.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    // MARK: - Bottom Pro Controls + Duplex Intercom Bar

    private func bottomControlBar(isLandscape: Bool, isCompactWidth: Bool) -> some View {
        HStack(spacing: 6) {
            compactPillButton(title: showManualDeck ? "DECK ●" : "DECK",
                              active: showManualDeck, tint: CineTheme.lavender) {
                withAnimation(.easeInOut(duration: 0.18)) {
                    showManualDeck.toggle()
                    if showManualDeck { showEncoderDrawer = false }
                }
            }

            compactPillButton(title: showEncoderDrawer ? "CODEC ●" : "CODEC",
                              active: showEncoderDrawer, tint: CineTheme.lavender) {
                withAnimation(.easeInOut(duration: 0.18)) {
                    showEncoderDrawer.toggle()
                    if showEncoderDrawer { showManualDeck = false }
                }
            }

            Spacer(minLength: 2)

            // Duplex Intercom PTT + Latch + IFB (strictly single-line pills)
            compactPillButton(
                title: engine.pttActive ? "🎙 ON" : "PTT",
                active: engine.pttActive,
                tint: engine.pttActive ? CineTheme.tallyRed : CineTheme.slate
            ) {
                engine.pttActive.toggle()
            }

            if !isCompactWidth || isLandscape {
                compactPillButton(title: "LATCH", active: engine.latchActive, tint: CineTheme.lavender) {
                    engine.latchActive.toggle()
                }
            }

            compactPillButton(title: engine.ifbEnabled ? "🎧 IFB" : "🔇",
                              active: engine.ifbEnabled, tint: CineTheme.tallyGreen) {
                engine.ifbEnabled.toggle()
            }

            if engine.isStreaming || engine.isDemoMode {
                Button {
                    engine.toggleStreaming()
                } label: {
                    Image(systemName: "stop.circle.fill")
                        .font(.system(size: 22))
                        .foregroundColor(CineTheme.tallyRed)
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(CineTheme.glass)
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(CineTheme.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    private func compactPillButton(title: String, active: Bool, tint: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .lineLimit(1)
                .fixedSize()
                .foregroundColor(active ? .white : CineTheme.lavenderSoft)
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(active ? tint : CineTheme.glassLight)
                .clipShape(Capsule())
        }
    }

    // MARK: - Mode 3: iPhone Duo Director / Focus Companion Desk

    private func duoCompanionDesk(isLandscape: Bool, safeTop: CGFloat, safeBot: CGFloat,
                                  safeLead: CGFloat, safeTrail: CGFloat) -> some View {
        ScrollView {
            VStack(spacing: 14) {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(tr("iPhone Duo — Пульт Режисера", "iPhone Duo — Director Companion"))
                            .font(.system(size: 18, weight: .bold))
                            .foregroundColor(CineTheme.textPrimary)
                        Text(tr("Бездротовий контроль фокусу, експозиції, Tally та інтеркому",
                                "Wireless Focus Pull, Exposure, Tally & Duplex Intercom"))
                            .font(.system(size: 11))
                            .foregroundColor(CineTheme.textMuted)
                    }
                    Spacer()
                    Button(tr("Видошукач", "Viewfinder")) { engine.setDuoMode(.singleHUD) }
                        .buttonStyle(.bordered)
                        .tint(CineTheme.lavenderBright)
                }

                // Tally Switcher
                HStack(spacing: 10) {
                    ForEach(ProTallyState.allCases, id: \.self) { st in
                        Button {
                            engine.tallyState = st
                            engine.remoteRec = (st == .program)
                            engine.sendCompanionCommandToSelectedPeer()
                        } label: {
                            Text(st.badgeText)
                                .font(.system(size: 12, weight: .bold, design: .monospaced))
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 12)
                                .background(engine.tallyState == st ? st.accentColor : CineTheme.glassLight)
                                .foregroundColor(.white)
                                .clipShape(RoundedRectangle(cornerRadius: 12))
                        }
                    }
                }

                // Precision Focus Pull A <-> B
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(tr("ФОКУС (RACK FOCUS A ↔ B)", "WIRELESS FOCUS PULL (A ↔ B)"))
                            .font(.system(size: 11, weight: .bold, design: .monospaced))
                            .foregroundColor(CineTheme.lavenderSoft)
                        Spacer()
                        Text("\(Int(engine.focusPosition * 100))%")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                            .foregroundColor(CineTheme.textPrimary)
                    }
                    Slider(value: $engine.focusPosition, in: 0...1)
                        .onChange(of: engine.focusPosition) { _ in
                            engine.autoFocus = false
                            engine.sendCompanionCommandToSelectedPeer()
                        }
                        .tint(CineTheme.lavenderBright)
                    HStack(spacing: 10) {
                        Button(tr("Точка A (25%)", "Mark A (25%)")) { engine.rackFocus(to: engine.focusPointA) }
                            .buttonStyle(.borderedProminent).tint(CineTheme.lavender).frame(maxWidth: .infinity)
                        Button(tr("Точка B (78%)", "Mark B (78%)")) { engine.rackFocus(to: engine.focusPointB) }
                            .buttonStyle(.borderedProminent).tint(CineTheme.lavender).frame(maxWidth: .infinity)
                    }
                }
                .padding(14)
                .background(CineTheme.glass)
                .clipShape(RoundedRectangle(cornerRadius: 16))

                manualDeckPanel(isCompactWidth: false)
                bottomControlBar(isLandscape: isLandscape, isCompactWidth: false)
            }
            .padding(.top, safeTop)
            .padding(.bottom, safeBot)
            .padding(.leading, safeLead)
            .padding(.trailing, safeTrail)
        }
    }

    // MARK: - Screenshot / Launch Argument Support

    private func applyLaunchScreenshotPresetIfNeeded() {
        let args = CommandLine.arguments
        if let idx = args.firstIndex(of: "-LVShot"), idx + 1 < args.count, let shot = Int(args[idx + 1]) {
            switch shot {
            case 1:
                // 1) Pro Cinema Viewfinder & Manual Controls
                engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                engine.focusPeaking = true
                showManualDeck = true
                showEncoderDrawer = false
            case 2:
                // 2) iPhone Duo Mode (Dual-Cam & Companion)
                engine.startDemoCineStudio(tally: .preview, mode: .dualCamSplit)
                showManualDeck = false
                showEncoderDrawer = false
            case 3:
                // 3) Live Studio Tally (PGM • ON AIR)
                engine.startDemoCineStudio(tally: .program, mode: .singleHUD)
                engine.remoteRec = true
                engine.directorSpeaking = true
                showManualDeck = false
                showEncoderDrawer = false
            case 4:
                // 4) Hardware Encoder & Duplex Intercom
                engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                engine.pttActive = true
                engine.latchActive = true
                engine.ifbEnabled = true
                showManualDeck = false
                showEncoderDrawer = true
            default:
                break
            }
        } else if args.contains("-LVDemo") {
            engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
            showManualDeck = true
        }
    }
}
