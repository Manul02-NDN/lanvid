import SwiftUI

/// LanVid Camera — Redesigned 1:1 in the clean neutral LanVid Studio design language:
/// Neutral dark/light surfaces (#1F1F1F / #262626 / #2E2E2E), hairline borders, single blue accent (#0A84FF),
/// segmented pill controls, 2x2 KPI monitoring tiles, live dual-line telemetry graph, vertical VU mixer strips,
/// multi-level hardware stabilization, and intuitive full-screen viewfinder touch gestures.
struct ProCineHUDView: View {
    @EnvironmentObject var engine: CameraEngine
    @Environment(\.studio) private var s

    @State private var activeDrawer: StudioDrawerTab? = nil
    @State private var showStabPopover = false
    @State private var showDuoSheet = false
    @State private var showSettingsSheet = false
    @State private var showBadgePopover = false

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height
            let isCompact = geo.size.width < 395 && !isLandscape
            let safeTop = max(geo.safeAreaInsets.top, isLandscape ? 10 : 12)
            let safeBot = max(geo.safeAreaInsets.bottom, isLandscape ? 10 : 12)
            let safeLead = max(geo.safeAreaInsets.leading, isLandscape ? 16 : 12)
            let safeTrail = max(geo.safeAreaInsets.trailing, isLandscape ? 16 : 12)

            ZStack {
                s.canvas.ignoresSafeArea()

                if engine.duoMode == .duoCompanion {
                    duoCompanionDesk(isLandscape: isLandscape, safeTop: safeTop, safeBot: safeBot,
                                     safeLead: safeLead, safeTrail: safeTrail)
                } else {
                    // Full-bleed interactive viewfinder with touch gestures
                    ViewfinderStageView(engine: engine, isLandscape: isLandscape) {
                        if showStabPopover || showBadgePopover {
                            withAnimation(.easeOut(duration: 0.15)) {
                                showStabPopover = false
                                showBadgePopover = false
                            }
                        }
                    }
                    .ignoresSafeArea()

                    // Crisp broadcast Tally frame border (only in PVW / PGM)
                    if engine.tallyState != .standby {
                        RoundedRectangle(cornerRadius: 24, style: .continuous)
                            .stroke(tallyColor, lineWidth: engine.tallyState == .program ? 6 : 4)
                            .ignoresSafeArea()
                            .allowsHitTesting(false)
                    }

                    if engine.cleanHUD {
                        cleanHudOverlay(safeTop: safeTop, safeBot: safeBot, safeLead: safeLead, safeTrail: safeTrail)
                    } else {
                        VStack(spacing: isCompact ? 6 : 8) {
                            topStudioBar(isLandscape: isLandscape, isCompact: isCompact)

                            // Contextual floating status badges (AE/AF lock, Duo sync, Touch toast)
                            floatingTopBadges(isCompact: isCompact)

                            Spacer(minLength: 4)

                            // Active expandable LanVid Studio Drawer OR unobstructed viewfinder quick controls
                            if let drawer = activeDrawer {
                                drawerContainer(for: drawer, maxHeight: isLandscape ? geo.size.height * 0.64 : geo.size.height * 0.56, isCompact: isCompact)
                                    .transition(.move(edge: .bottom).combined(with: .opacity))
                            } else {
                                if showStabPopover {
                                    stabilizationQuickPopover
                                        .transition(.move(edge: .bottom).combined(with: .opacity))
                                }
                                viewfinderQuickStrip(isCompact: isCompact)
                            }

                            bottomStudioDock(isLandscape: isLandscape, isCompact: isCompact)
                        }
                        .padding(.top, safeTop)
                        .padding(.bottom, safeBot)
                        .padding(.leading, safeLead)
                        .padding(.trailing, safeTrail)
                    }
                }
            }
            .ignoresSafeArea()
        }
        .sheet(isPresented: $showDuoSheet) {
            DuoSyncSheetView()
                .environmentObject(engine)
                .environment(\.studio, s)
        }
        .sheet(isPresented: $showSettingsSheet) {
            CameraSettingsView()
                .environmentObject(engine)
                .environment(\.studio, s)
        }
        .onAppear {
            applyLaunchScreenshotPresetIfNeeded()
        }
    }

    private var tallyColor: Color {
        switch engine.tallyState {
        case .program: return s.red
        case .preview: return s.green
        case .standby: return engine.isStreaming ? s.green : (engine.isIntercomActive ? s.accent : s.text3)
        }
    }

    // MARK: - Clean Viewfinder Mode (minimal chrome)

    private func cleanHudOverlay(safeTop: CGFloat, safeBot: CGFloat, safeLead: CGFloat, safeTrail: CGFloat) -> some View {
        VStack {
            HStack {
                StudioStatusPill(
                    dotColor: tallyColor,
                    title: engine.displayBadgeTag,
                    detail: "\(engine.resolution.rawValue) \(engine.fps)p · \(String(format: "%.1f×", engine.zoomFactor))"
                )
                Spacer()
                StudioIconButton(systemName: "eye.fill", active: true) {
                    withAnimation(.easeInOut(duration: 0.18)) { engine.cleanHUD = false }
                }
            }
            Spacer()
            if let toast = engine.touchHUD.toastText {
                touchToastPill(text: toast, icon: engine.touchHUD.toastIcon)
                    .padding(.bottom, 12)
            }
        }
        .padding(.top, safeTop)
        .padding(.bottom, safeBot)
        .padding(.leading, safeLead)
        .padding(.trailing, safeTrail)
    }

    // MARK: - Top LanVid Studio Header Bar (matches ref1–ref4 titlebar)

    private func topStudioBar(isLandscape: Bool, isCompact: Bool) -> some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                // Brand mark + Status/Tally pill
                HStack(spacing: 6) {
                    LanVidBrandMark(size: 23)
                    if isLandscape || (!isCompact && !engine.remoteRec) {
                        Text("LanVid")
                            .font(.system(size: 13.5, weight: .semibold))
                            .foregroundColor(s.text)
                            .tracking(-0.2)
                            .lineLimit(1)
                            .fixedSize()
                    }
                }

                // Online / Tally pill (exact .status-pill)
                Button {
                    if engine.isDemoMode {
                        engine.cycleDemoTally()
                    } else {
                        withAnimation(.easeInOut(duration: 0.15)) { showBadgePopover.toggle() }
                    }
                } label: {
                    let pillLabel: String = {
                        if engine.tallyState != .standby {
                            return "\(engine.displayBadgeTag) · \(engine.tallyState.shortCode)"
                        }
                        if engine.isStreaming {
                            return "\(engine.displayBadgeTag) · \(tr("Онлайн", "Online"))"
                        }
                        if engine.isIntercomActive {
                            return "\(engine.displayBadgeTag) · \(tr("Інтерком", "Intercom"))"
                        }
                        return engine.displayBadgeTag
                    }()
                    let detailText: String? = isLandscape
                        ? String(format: "%@ %dp · %.1f Мбіт/с · %.0f мс", engine.resolution.rawValue, engine.fps, engine.telemetry.bitrateMbps, engine.telemetry.latencyMs)
                        : nil
                    StudioStatusPill(dotColor: tallyColor, title: pillLabel, detail: detailText)
                }
                .buttonStyle(.plain)

                if engine.remoteRec {
                    StudioBadge(text: "● REC", tone: .red)
                }
                if engine.isIntercomActive && !engine.directorSpeaking && (isLandscape || !isCompact) {
                    StudioBadge(text: "IFB", tone: .green)
                }
                if engine.directorSpeaking && isLandscape {
                    StudioBadge(text: tr("Режисер", "Director"), tone: .accent)
                }

                Spacer(minLength: 2)

                // Right icon controls (Duo, Theme toggle, Clean HUD, Settings — matching ref1–ref4 top-right)
                HStack(spacing: 5) {
                    StudioIconButton(
                        systemName: "rectangle.split.2x1",
                        active: engine.duoMode != .singleHUD,
                        size: 30
                    ) { showDuoSheet = true }

                    StudioIconButton(
                        systemName: engine.themeMode == .dark ? "moon" : "sun.max",
                        size: 30
                    ) { engine.toggleTheme() }

                    StudioIconButton(
                        systemName: "eye",
                        size: 30
                    ) {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            activeDrawer = nil
                            showStabPopover = false
                            engine.cleanHUD = true
                        }
                    }

                    StudioIconButton(
                        systemName: "gearshape",
                        size: 30
                    ) { showSettingsSheet = true }
                }
            }

            // Secondary compact telemetry bar in Portrait (timecode + format + bitrate + latency + battery/temp)
            if !isLandscape {
                HStack(spacing: 6) {
                    Text(engine.telemetry.timecode)
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text)
                        .fixedSize()

                    Text("·").foregroundColor(s.text3)

                    Text(String(format: "%@ %dp · %.1fM · %.0f%@",
                                engine.resolution.rawValue, engine.fps,
                                engine.telemetry.bitrateMbps, engine.telemetry.latencyMs,
                                tr("мс", "ms")))
                        .font(.system(size: 10.5, weight: .medium).monospacedDigit())
                        .foregroundColor(s.text2)
                        .lineLimit(1)

                    if engine.directorSpeaking {
                        HStack(spacing: 3) {
                            Image(systemName: "mic.fill")
                                .font(.system(size: 9, weight: .bold))
                            Text(tr("Режисер", "Dir"))
                                .font(.system(size: 10, weight: .semibold))
                        }
                        .foregroundColor(s.accent)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(s.accentSoft))
                        .fixedSize()
                    } else if engine.isIntercomActive {
                        HStack(spacing: 3) {
                            Image(systemName: "headphones")
                                .font(.system(size: 9, weight: .bold))
                            Text("IFB")
                                .font(.system(size: 10, weight: .semibold))
                        }
                        .foregroundColor(s.green)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(s.greenSoft))
                        .fixedSize()
                    }

                    Spacer(minLength: 2)

                    // Compact battery & temp
                    HStack(spacing: 4) {
                        Text("\(engine.telemetry.tempC)°C")
                            .foregroundColor(engine.telemetry.tempC >= 45 ? s.amber : s.text3)
                        Text("\(engine.telemetry.batteryPct)%")
                            .foregroundColor(engine.telemetry.batteryPct <= 15 ? s.red : s.text2)
                    }
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .fixedSize()
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(
            RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous)
                .fill(s.hudBar)
        )
        .overlay(
            RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous)
                .stroke(s.borderStrong, lineWidth: 0.75)
        )
    }

    // MARK: - Floating Top Badges (AE/AF Lock, Studio Sync Popover, Touch Toast, Duo Banner)

    @ViewBuilder
    private func floatingTopBadges(isCompact: Bool) -> some View {
        VStack(spacing: 6) {
            if showBadgePopover {
                HStack(spacing: 8) {
                    Circle().fill(tallyColor).frame(width: 7, height: 7)
                    let modeStr = engine.syncedFixedDelay ? "\(engine.syncedDelayMs) \(tr("мс", "ms"))" : tr("Авто", "Auto")
                    Text(engine.configSynced
                         ? tr("Призначено студією: \(engine.displayBadgeTag) · Буфер \(modeStr)",
                              "Assigned by Studio: \(engine.displayBadgeTag) · Buffer \(modeStr)")
                         : tr("Камера \(engine.displayBadgeTag) · \(engine.destIp):\(engine.videoPort)",
                              "Camera \(engine.displayBadgeTag) · \(engine.destIp):\(engine.videoPort)"))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(s.text)
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    Button(tr("Мережа", "Devices")) {
                        showBadgePopover = false
                        withAnimation(.easeInOut(duration: 0.18)) { activeDrawer = .devices }
                    }
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(s.accent)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.hudSurface))
                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.borderStrong, lineWidth: 0.75))
            }

            if engine.duoMode.isMultiCam || !engine.browser.duoPeers.isEmpty {
                HStack(spacing: 7) {
                    Image(systemName: "iphone.radiowaves.left.and.right")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(s.accent)
                    Text(engine.duoMode.title)
                        .font(.system(size: 11.5, weight: .semibold))
                        .foregroundColor(s.text)
                    if let peer = engine.browser.duoPeers.first {
                        StudioBadge(text: "\(peer.role.rawValue) · \(peer.tally.shortCode)",
                                    tone: peer.tally == .program ? .red : (peer.tally == .preview ? .green : .neutral))
                    }
                    Spacer(minLength: 4)
                    Button(tr("Змінити", "Switch")) { showDuoSheet = true }
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(s.accent)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.hudBar))
                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.border, lineWidth: 0.75))
            }

            HStack(spacing: 6) {
                if engine.aeAfLocked {
                    Button {
                        engine.unlockAeAf()
                    } label: {
                        HStack(spacing: 5) {
                            Image(systemName: "lock.fill")
                                .font(.system(size: 10, weight: .bold))
                            Text("AE / AF LOCK")
                                .font(.system(size: 10.5, weight: .semibold, design: .monospaced))
                            Image(systemName: "xmark")
                                .font(.system(size: 9, weight: .bold))
                                .opacity(0.75)
                        }
                        .foregroundColor(s.amber)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Capsule().fill(s.hudSurface))
                        .overlay(Capsule().stroke(s.amber.opacity(0.65), lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                }

                if !engine.autoFocus && !engine.aeAfLocked {
                    Button {
                        engine.autoFocus = true
                        engine.applyManualCameraControls()
                        engine.showTouchToast(tr("Автофокус увімкнено", "Auto Focus On"), icon: "viewfinder")
                    } label: {
                        HStack(spacing: 4) {
                            Text("MF \(Int(engine.focusPosition * 100))%")
                                .font(.system(size: 10.5, weight: .semibold, design: .monospaced))
                            Text("→ AF")
                                .font(.system(size: 10, weight: .bold))
                                .foregroundColor(s.accent)
                        }
                        .foregroundColor(s.text)
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(Capsule().fill(s.hudSurface))
                        .overlay(Capsule().stroke(s.borderStrong, lineWidth: 0.75))
                    }
                    .buttonStyle(.plain)
                }

                if abs(engine.exposureBias) >= 0.05 {
                    Button {
                        engine.setExposureBias(0.0)
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "sun.max.fill")
                                .font(.system(size: 10))
                                .foregroundColor(s.yellow)
                            Text(String(format: "%+.1f EV", engine.exposureBias))
                                .font(.system(size: 10.5, weight: .semibold, design: .monospaced))
                                .foregroundColor(s.text)
                        }
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(Capsule().fill(s.hudSurface))
                        .overlay(Capsule().stroke(s.borderStrong, lineWidth: 0.75))
                    }
                    .buttonStyle(.plain)
                }
            }

            if let toast = engine.touchHUD.toastText {
                touchToastPill(text: toast, icon: engine.touchHUD.toastIcon)
                    .transition(.opacity.combined(with: .scale(scale: 0.94)))
            }
        }
    }

    private func touchToastPill(text: String, icon: String) -> some View {
        HStack(spacing: 6) {
            Image(systemName: icon)
                .font(.system(size: 11, weight: .semibold))
            Text(text)
                .font(.system(size: 12, weight: .semibold))
                .monospacedDigit()
        }
        .foregroundColor(s.primaryFg)
        .padding(.horizontal, 13)
        .padding(.vertical, 6)
        .background(Capsule().fill(s.primaryBg))
        .shadow(color: Color.black.opacity(0.32), radius: 10, y: 4)
    }

    // MARK: - Viewfinder Quick Strip (Stabilization Pill + Zoom Wheel Pill + Flip + Demo Quick Pill)

    private func viewfinderQuickStrip(isCompact: Bool) -> some View {
        VStack(spacing: 8) {
            // Subtle idle quick-connect strip when not live (leaves viewfinder completely open!)
            if !engine.isStreaming && !engine.isDemoMode {
                HStack(spacing: 8) {
                    Circle().fill(engine.browser.studios.isEmpty ? s.amber : s.green)
                        .frame(width: 7, height: 7)
                    if let first = engine.browser.studios.first {
                        Text("\(first.name) · \(first.host)")
                            .font(.system(size: 11.5, weight: .medium))
                            .foregroundColor(s.text)
                            .lineLimit(1)
                    } else {
                        Text("\(tr("Студія", "Studio")): \(engine.destIp):\(engine.videoPort)")
                            .font(.system(size: 11.5, weight: .medium, design: .monospaced))
                            .foregroundColor(s.text2)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 4)
                    Button {
                        engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "sparkles.tv")
                                .font(.system(size: 11))
                            Text(tr("Демо", "Demo"))
                                .font(.system(size: 11.5, weight: .semibold))
                        }
                        .foregroundColor(s.text)
                        .padding(.horizontal, 10)
                        .frame(height: 26)
                        .background(Capsule().fill(s.surface2))
                        .overlay(Capsule().stroke(s.borderStrong, lineWidth: 0.75))
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.hudBar))
                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))
            }

            // Main tactile quick strip: Stabilization | Zoom/Lenses | Flip
            HStack(spacing: 8) {
                // Quick Stabilization pill (tap opens 5-level selector)
                Button {
                    withAnimation(.spring(response: 0.22, dampingFraction: 0.85)) {
                        showStabPopover.toggle()
                    }
                } label: {
                    HStack(spacing: 5) {
                        Image(systemName: engine.stabilizationLevel.systemIcon)
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundColor(engine.stabilizationLevel == .off ? s.text3 : s.accent)
                        Text(engine.stabilizationLevel.title)
                            .font(.system(size: 11.5, weight: .semibold))
                            .foregroundColor(engine.stabilizationLevel == .off ? s.text2 : s.text)
                            .lineLimit(1)
                    }
                    .padding(.horizontal, 10)
                    .frame(height: 32)
                    .background(
                        Capsule().fill(showStabPopover ? s.accentSoft : s.hudBar)
                    )
                    .overlay(
                        Capsule().stroke(showStabPopover ? s.accent : s.borderStrong, lineWidth: 0.9)
                    )
                }
                .buttonStyle(.plain)

                Spacer(minLength: 2)

                // LanVid Studio Zoom Pill (.zoom-pill from ref1 & ref2!)
                HStack(spacing: 2) {
                    Button { engine.stepZoom(up: false) } label: {
                        Text("−")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundColor(s.text2)
                            .frame(width: 26, height: 26)
                    }
                    .buttonStyle(.plain)

                    ForEach([CineLens.ultraWide, .wide, .tele, .tele3x]) { preset in
                        let isSelected = (engine.lens == preset)
                        let showLiveZoom = isSelected && abs(engine.zoomFactor - preset.targetDisplayZoom) > 0.09
                        Button {
                            engine.selectLens(preset)
                        } label: {
                            Text(showLiveZoom ? String(format: "%.1f×", engine.zoomFactor) : preset.rawValue.replacingOccurrences(of: "x", with: "×"))
                                .font(.system(size: 11, weight: isSelected ? .bold : .medium, design: .monospaced))
                                .foregroundColor(isSelected ? s.accent : s.text2)
                                .padding(.horizontal, isCompact ? 6 : 8)
                                .frame(height: 26)
                                .background(
                                    Capsule().fill(isSelected ? s.accentSoft : Color.clear)
                                )
                        }
                        .buttonStyle(.plain)
                    }

                    Button { engine.stepZoom(up: true) } label: {
                        Text("+")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundColor(s.text2)
                            .frame(width: 26, height: 26)
                    }
                    .buttonStyle(.plain)
                }
                .padding(3)
                .background(Capsule().fill(s.hudBar))
                .overlay(Capsule().stroke(s.borderStrong, lineWidth: 0.85))

                Spacer(minLength: 2)

                // Quick Front / Back Camera flip
                StudioIconButton(
                    systemName: "arrow.triangle.2.circlepath.camera",
                    active: engine.lens == .front,
                    size: 32,
                    bordered: true
                ) {
                    engine.selectLens(engine.lens == .front ? .wide : .front)
                }
            }
        }
    }

    // MARK: - Inline 5-Level Stabilization Quick Popover

    private var stabilizationQuickPopover: some View {
        StudioCard(translucent: true, padding: 12) {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: "gyroscope")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(s.accent)
                    Text(tr("Стабілізація зображення", "Image Stabilization"))
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundColor(s.text)
                    Spacer()
                    StudioBadge(
                        text: tr("Апаратно: ", "HW: ") + engine.activeHardwareStabilization.title,
                        tone: engine.activeHardwareStabilization == .off ? .neutral : .green
                    )
                }

                StudioSegmentedControl(
                    items: StabilizationLevel.allCases,
                    selection: Binding(
                        get: { engine.stabilizationLevel },
                        set: { engine.setStabilizationLevel($0) }
                    ),
                    title: { $0.title },
                    compact: true
                )

                HStack {
                    Text(engine.stabilizationLevel.subtitle)
                        .font(.system(size: 11))
                        .foregroundColor(s.text2)
                    Spacer()
                }
            }
        }
    }

    // MARK: - Expandable LanVid Studio Contextual Drawers

    private func drawerContainer(for tab: StudioDrawerTab, maxHeight: CGFloat, isCompact: Bool) -> some View {
        StudioCard(translucent: true, padding: 0) {
            VStack(spacing: 0) {
                // Drawer header with close button
                HStack {
                    Image(systemName: tab.icon)
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundColor(s.accent)
                    Text(drawerTitle(for: tab))
                        .font(.system(size: 13.5, weight: .semibold))
                        .foregroundColor(s.text)
                    Spacer()
                    if engine.isDemoMode && tab == .camera {
                        StudioPresetChip(title: tr("Тест Tally", "Cycle Tally"), active: false) {
                            engine.cycleDemoTally()
                        }
                    }
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) { activeDrawer = nil }
                    } label: {
                        Image(systemName: "chevron.down")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundColor(s.text2)
                            .frame(width: 26, height: 26)
                            .background(Circle().fill(s.surface2))
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 10)

                Divider().overlay(s.border)

                ScrollView(.vertical, showsIndicators: false) {
                    Group {
                        switch tab {
                        case .camera:  cameraDrawerBody(isCompact: isCompact)
                        case .quality: qualityDrawerBody(isCompact: isCompact)
                        case .monitor: monitorDrawerBody(isCompact: isCompact)
                        case .devices: devicesDrawerBody(isCompact: isCompact)
                        }
                    }
                    .padding(14)
                }
                .frame(maxHeight: maxHeight)
            }
        }
    }

    private func drawerTitle(for tab: StudioDrawerTab) -> String {
        switch tab {
        case .camera:  return tr("Камера і стабілізація", "Camera & Stabilization")
        case .quality: return tr("Якість і режим передачі", "Quality & Transmission")
        case .monitor: return tr("Моніторинг і телеметрія", "Monitoring & Telemetry")
        case .devices: return tr("Пристрої у мережі", "Network Devices")
        }
    }

    // MARK: - Drawer 1: Камера і стабілізація

    private func cameraDrawerBody(isCompact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            // Stabilization levels section (exact grouped row aesthetic from ref4)
            VStack(alignment: .leading, spacing: 7) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(tr("Стабілізація зображення", "Image Stabilization"))
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(s.text)
                        Text(engine.stabilizationLevel.subtitle)
                            .font(.system(size: 11))
                            .foregroundColor(s.text3)
                    }
                    Spacer()
                    StudioBadge(
                        text: engine.activeHardwareStabilization.shortBadge,
                        tone: engine.activeHardwareStabilization == .off ? .neutral : .accent
                    )
                }
                StudioSegmentedControl(
                    items: StabilizationLevel.allCases,
                    selection: Binding(
                        get: { engine.stabilizationLevel },
                        set: { engine.setStabilizationLevel($0) }
                    ),
                    title: { $0.title },
                    compact: true
                )
            }

            Divider().overlay(s.border)

            // Lens & continuous zoom
            VStack(alignment: .leading, spacing: 7) {
                HStack {
                    Text(tr("Об'єктив і зум", "Lens & Zoom"))
                        .font(.system(size: 12.5, weight: .medium))
                        .foregroundColor(s.text)
                    Spacer()
                    Text(String(format: "%.1f×", engine.zoomFactor))
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.accent)
                }
                StudioSegmentedControl(
                    items: CineLens.allCases,
                    selection: Binding(
                        get: { engine.lens },
                        set: { engine.selectLens($0) }
                    ),
                    title: { $0.displayLabel },
                    compact: true
                )
                Slider(
                    value: Binding(
                        get: { Double(engine.zoomFactor) },
                        set: { engine.setDisplayZoom(CGFloat($0), animated: false, showOverlay: false) }
                    ),
                    in: Double(engine.minDisplayZoom)...Double(engine.maxDisplayZoom)
                )
                .tint(s.accent)
            }

            Divider().overlay(s.border)

            // Exposure: EV compensation + Shutter Angle + ISO
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(tr("Експозиція", "Exposure"))
                        .font(.system(size: 12.5, weight: .medium))
                        .foregroundColor(s.text)
                    Spacer()
                    StudioPresetChip(
                        title: engine.autoExposure ? tr("Авто (AE)", "Auto (AE)") : tr("Ручна (M)", "Manual (M)"),
                        active: !engine.autoExposure
                    ) {
                        engine.autoExposure.toggle()
                        engine.applyManualCameraControls()
                    }
                    StudioPresetChip(
                        title: engine.aeAfLocked ? tr("Заблоковано", "Locked") : tr("AE/AF Lock", "AE/AF Lock"),
                        active: engine.aeAfLocked
                    ) {
                        if engine.aeAfLocked {
                            engine.unlockAeAf()
                        } else {
                            engine.lockFocusAndExposure(atDevicePoint: CGPoint(x: 0.5, y: 0.5),
                                                        viewNormalizedPoint: CGPoint(x: 0.5, y: 0.5))
                        }
                    }
                }

                // EV Compensation slider
                HStack(spacing: 8) {
                    Text(String(format: "EV %+.1f", engine.exposureBias))
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .frame(width: 64, alignment: .leading)
                    Slider(
                        value: Binding(
                            get: { engine.exposureBias },
                            set: { engine.setExposureBias($0, activeDrag: false) }
                        ),
                        in: -3.0...3.0,
                        step: 0.1
                    )
                    .tint(s.accent)
                    Button("0.0") { engine.setExposureBias(0.0) }
                        .font(.system(size: 10.5, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .padding(.horizontal, 7)
                        .frame(height: 22)
                        .background(RoundedRectangle(cornerRadius: 6).fill(s.surface2))
                }

                // Shutter Angle pills
                HStack(spacing: 6) {
                    Text("SHUTTER")
                        .font(.system(size: 10.5, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text3)
                        .frame(width: 64, alignment: .leading)
                    ForEach([45, 90, 180, 360], id: \.self) { ang in
                        StudioPresetChip(title: "\(ang)°", active: !engine.autoExposure && engine.shutterAngle == ang) {
                            engine.autoExposure = false
                            engine.shutterAngle = ang
                            engine.applyManualCameraControls()
                        }
                    }
                }

                // ISO slider
                HStack(spacing: 8) {
                    Text("ISO \(Int(engine.isoValue))")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .frame(width: 64, alignment: .leading)
                    Slider(value: $engine.isoValue, in: 50...3200, step: 50)
                        .onChange(of: engine.isoValue) { _ in
                            engine.autoExposure = false
                            engine.applyManualCameraControls()
                        }
                        .tint(s.accent)
                }
            }

            Divider().overlay(s.border)

            // White Balance + Focus A <-> B
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Text(String(format: "WB %dK", Int(engine.wbKelvin)))
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .frame(width: 72, alignment: .leading)
                    Slider(value: $engine.wbKelvin, in: 2500...9500, step: 100)
                        .onChange(of: engine.wbKelvin) { _ in
                            engine.autoWhiteBalance = false
                            engine.applyManualCameraControls()
                        }
                        .tint(s.amber)
                    StudioPresetChip(title: engine.autoWhiteBalance ? "AWB" : "M", active: !engine.autoWhiteBalance) {
                        engine.autoWhiteBalance.toggle()
                        engine.applyManualCameraControls()
                    }
                }

                HStack(spacing: 8) {
                    Text(engine.autoFocus ? "AF" : "MF \(Int(engine.focusPosition * 100))%")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .frame(width: 72, alignment: .leading)
                    Slider(value: $engine.focusPosition, in: 0...1)
                        .onChange(of: engine.focusPosition) { _ in
                            engine.autoFocus = false
                            engine.applyManualCameraControls()
                        }
                        .tint(s.accent)
                    StudioPresetChip(title: "A", active: false) { engine.rackFocus(to: engine.focusPointA) }
                    StudioPresetChip(title: "B", active: false) { engine.rackFocus(to: engine.focusPointB) }
                }
            }

            if engine.duoMode.isMultiCam {
                Divider().overlay(s.border)
                HStack(spacing: 8) {
                    Text(String(format: "FRONT %+.1f", engine.secondaryExposureBias))
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(s.text2)
                        .frame(width: 78, alignment: .leading)
                    Slider(value: $engine.secondaryExposureBias, in: -2.0...2.0, step: 0.2)
                        .onChange(of: engine.secondaryExposureBias) { _ in engine.applyManualCameraControls() }
                        .tint(s.green)
                }
            }

            Divider().overlay(s.border)

            // Viewfinder assists row
            HStack(spacing: 6) {
                StudioPresetChip(title: tr("Сітка", "Guides"), active: engine.showGuides) { engine.showGuides.toggle() }
                StudioPresetChip(title: tr("Пікінг", "Peaking"), active: engine.focusPeaking) { engine.focusPeaking.toggle() }
                StudioPresetChip(title: tr("Зебра", "Zebra"), active: engine.zebra) { engine.zebra.toggle() }
                Spacer()
            }
        }
    }

    // MARK: - Drawer 2: Якість і режим передачі (1:1 port of ref4)

    private func qualityDrawerBody(isCompact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            // Codec row (like "Формат аудіо" in ref4)
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(tr("Відеокодек", "Video Codec"))
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(s.text)
                        Text(engine.telemetry.codecBackend)
                            .font(.system(size: 11))
                            .foregroundColor(s.text3)
                    }
                    Spacer()
                    StudioBadge(text: "Zero B-Frames", tone: .green)
                }
                StudioSegmentedControl(
                    items: CineCodec.allCases,
                    selection: $engine.codec,
                    title: { $0.shortName },
                    compact: true
                )
            }

            Divider().overlay(s.border)

            // Resolution & FPS row
            VStack(alignment: .leading, spacing: 7) {
                Text(tr("Роздільна здатність і частота кадрів", "Resolution & Frame Rate"))
                    .font(.system(size: 12.5, weight: .medium))
                    .foregroundColor(s.text)
                HStack(spacing: 8) {
                    StudioSegmentedControl(
                        items: CineResolution.allCases,
                        selection: $engine.resolution,
                        title: { $0.rawValue },
                        compact: true
                    )
                    StudioSegmentedControl(
                        items: [FpsOption(24), FpsOption(30), FpsOption(60)],
                        selection: Binding(
                            get: { FpsOption(engine.fps) },
                            set: { engine.fps = $0.value }
                        ),
                        title: { "\($0.value)p" },
                        compact: true
                    )
                }
            }

            Divider().overlay(s.border)

            // Bitrate slider + stepper value pill (exact match to "Затримка буфера" in ref4!)
            VStack(alignment: .leading, spacing: 7) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(tr("Бітрейт потоку", "Stream Bitrate"))
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundColor(s.text)
                        Text(String(format: "%d×%d · %d fps · %@", engine.resolution.width, engine.resolution.height, engine.fps, engine.colorProfile))
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundColor(s.text3)
                    }
                    Spacer()
                    // Numeric stepper box (.lat-stepper)
                    HStack(spacing: 6) {
                        Text("\(Int(engine.bitrateMbps)) \(tr("Мбіт/с", "Mbps"))")
                            .font(.system(size: 12, weight: .semibold).monospacedDigit())
                            .foregroundColor(s.text)
                        VStack(spacing: 2) {
                            Button { engine.bitrateMbps = min(100, engine.bitrateMbps + 5) } label: {
                                Image(systemName: "chevron.up").font(.system(size: 8, weight: .bold))
                                    .foregroundColor(s.text2)
                                    .frame(width: 18, height: 11)
                                    .background(RoundedRectangle(cornerRadius: 3).fill(s.surface))
                            }.buttonStyle(.plain)
                            Button { engine.bitrateMbps = max(5, engine.bitrateMbps - 5) } label: {
                                Image(systemName: "chevron.down").font(.system(size: 8, weight: .bold))
                                    .foregroundColor(s.text2)
                                    .frame(width: 18, height: 11)
                                    .background(RoundedRectangle(cornerRadius: 3).fill(s.surface))
                            }.buttonStyle(.plain)
                        }
                    }
                    .padding(.leading, 9)
                    .padding(.trailing, 4)
                    .padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.surface2))
                    .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.borderStrong, lineWidth: 0.75))
                }

                Slider(value: $engine.bitrateMbps, in: 5...100, step: 5)
                    .tint(s.accent)

                // Bitrate presets (exact .lat-preset-chip style from ref4!)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        StudioPresetChip(title: "Wi-Fi", subtitle: "15M", active: Int(engine.bitrateMbps) == 15) { engine.bitrateMbps = 15 }
                        StudioPresetChip(title: tr("Студія (LAN)", "Studio (LAN)"), subtitle: "35M", active: Int(engine.bitrateMbps) == 35) { engine.bitrateMbps = 35 }
                        StudioPresetChip(title: tr("Майстер", "Master"), subtitle: "80M", active: Int(engine.bitrateMbps) == 80) { engine.bitrateMbps = 80 }
                    }
                }
            }

            Divider().overlay(s.border)

            // Protection & Sync toggles (exact right column of ref4!)
            VStack(spacing: 8) {
                Toggle(isOn: $engine.adaptiveQuality) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(tr("Адаптивна якість (AIMD)", "Adaptive Quality (AIMD)"))
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundColor(s.text)
                        Text(tr("Автопідлаштування під пропускну здатність Wi-Fi", "Auto-adapts to Wi-Fi throughput"))
                            .font(.system(size: 10.5))
                            .foregroundColor(s.text3)
                    }
                }
                .tint(s.accent)

                Toggle(isOn: $engine.fecEnabled) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(tr("Корекція помилок (XOR FEC)", "Forward Error Correction (FEC)"))
                            .font(.system(size: 12.5, weight: .medium))
                            .foregroundColor(s.text)
                        Text(tr("Відновлення втрачених UDP-пакетів без повторів", "Recovers dropped UDP packets in flight"))
                            .font(.system(size: 10.5))
                            .foregroundColor(s.text3)
                    }
                }
                .tint(s.accent)
            }
        }
    }

    // MARK: - Drawer 3: Моніторинг і телеметрія (1:1 port of ref3 & ref4!)

    private func monitorDrawerBody(isCompact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            // 2x2 KPI Grid (.net-kpi-grid from ref3 & ref4)
            let cols = [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)]
            LazyVGrid(columns: cols, spacing: 8) {
                StudioKpiTile(
                    title: tr("Мережевий трафік", "Network Traffic"),
                    trailingTag: engine.codec.shortName,
                    value: String(format: "%.1f", engine.telemetry.bitrateMbps),
                    unit: tr("Мбіт/с", "Mbps"),
                    subtitle: "\(engine.resolution.rawValue) · \(Int(engine.telemetry.actualFps)) fps",
                    progress: min(1.0, engine.telemetry.bitrateMbps / 100.0),
                    barColor: s.accent
                )
                StudioKpiTile(
                    title: tr("Затримка і джиттер", "Latency & Jitter"),
                    trailingTag: engine.syncedFixedDelay ? tr("Фікс.", "Fixed") : tr("Авто", "Auto"),
                    value: String(format: "%.1f", engine.telemetry.latencyMs),
                    unit: tr("мс", "ms"),
                    subtitle: String(format: "%@ ±%.2f %@", tr("Джиттер", "Jitter"), engine.telemetry.jitterMs, tr("мс", "ms")),
                    progress: min(1.0, engine.telemetry.latencyMs / 40.0),
                    barColor: s.violet
                )
                StudioKpiTile(
                    title: tr("Втрати та буфер", "Loss & Buffer"),
                    trailingTag: "\(engine.telemetry.bufferPct)%",
                    value: String(format: "%.2f", engine.telemetry.lossPct),
                    unit: "%",
                    subtitle: tr("Провали: 0 · FEC активний", "Drops: 0 · FEC active"),
                    progress: max(0.05, 1.0 - engine.telemetry.lossPct / 5.0),
                    barColor: engine.telemetry.lossPct > 0.5 ? s.amber : s.green
                )
                StudioKpiTile(
                    title: tr("Стабілізація", "Stabilization"),
                    trailingTag: "\(engine.telemetry.tempC)°C",
                    value: engine.activeHardwareStabilization.title,
                    unit: "",
                    subtitle: tr("Кроп ~\(engine.activeHardwareStabilization.cropPercent)% · Бат. \(engine.telemetry.batteryPct)%",
                                 "Crop ~\(engine.activeHardwareStabilization.cropPercent)% · Bat \(engine.telemetry.batteryPct)%"),
                    progress: max(0.08, engine.activeHardwareStabilization.levelProgress),
                    barColor: engine.activeHardwareStabilization == .off ? s.text3 : s.green
                )
            }

            // Live Dual-Line Telemetry Graph (.net-chart-card from ref3 & ref4)
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(tr("Трафік і затримка в реальному часі", "Real-Time Traffic & Latency"))
                        .font(.system(size: 11.5, weight: .semibold))
                        .foregroundColor(s.text2)
                    Spacer()
                    HStack(spacing: 10) {
                        HStack(spacing: 4) {
                            Circle().fill(s.accent).frame(width: 6, height: 6)
                            Text(tr("Трафік", "Traffic")).font(.system(size: 10)).foregroundColor(s.text3)
                        }
                        HStack(spacing: 4) {
                            Circle().fill(s.violet).frame(width: 6, height: 6)
                            Text(tr("Затримка", "Latency")).font(.system(size: 10)).foregroundColor(s.text3)
                        }
                    }
                }

                liveTelemetryGraph
                    .frame(height: 86)
                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.bgSubtle))
                    .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.border, lineWidth: 0.75))
                    .clipShape(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous))

                // Diagnostics pill strip (.net-diag-strip from ref3 & ref4)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        diagPill(label: tr("Буфер", "Buffer"), value: "\(engine.telemetry.bufferPct)%", accent: s.green)
                        diagPill(label: tr("Захист", "Protect"), value: engine.fecEnabled ? "XOR FEC" : "Off", accent: s.text)
                        diagPill(label: "TC", value: engine.telemetry.timecode, accent: s.text)
                    }
                }
            }
            .padding(11)
            .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
            .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))

            // Independent Intercom Master Control Card (works WITH or WITHOUT live video!)
            HStack(spacing: 10) {
                Circle()
                    .fill(engine.isIntercomActive ? s.green : s.text3)
                    .frame(width: 8, height: 8)
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text(tr("Інтерком (незалежно від відео)", "Intercom (Independent)"))
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundColor(s.text)
                            .lineLimit(1)
                        if engine.directorSpeaking {
                            StudioBadge(text: tr("Режисер", "Director"), tone: .accent)
                        }
                    }
                    Text(engine.isIntercomActive
                         ? "\(engine.destIp):\(engine.tallyPort) · \(engine.isStreaming ? tr("з відеопотоком", "with video") : tr("лише інтерком", "intercom only"))"
                         : tr("Вимкнено · можна увімкнути без стріму відео", "Off · can run without video stream"))
                        .font(.system(size: 10.5, design: engine.isIntercomActive ? .monospaced : .default))
                        .foregroundColor(s.text3)
                        .lineLimit(1)
                }
                Spacer(minLength: 4)
                Button {
                    engine.toggleIntercom()
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: engine.isIntercomActive ? "headphones" : "power")
                            .font(.system(size: 11, weight: .semibold))
                        Text(engine.isIntercomActive ? tr("Вимк", "Off") : tr("Увімк", "On"))
                            .font(.system(size: 11.5, weight: .semibold))
                    }
                    .foregroundColor(engine.isIntercomActive ? s.green : s.primaryFg)
                    .padding(.horizontal, 10)
                    .frame(height: 30)
                    .background(
                        RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous)
                            .fill(engine.isIntercomActive ? s.greenSoft : s.primaryBg)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous)
                            .stroke(engine.isIntercomActive ? s.green.opacity(0.55) : Color.clear, lineWidth: 0.75)
                    )
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 11)
            .padding(.vertical, 9)
            .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
            .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(engine.isIntercomActive ? s.green.opacity(0.45) : s.border, lineWidth: 0.75))

            // Vertical Intercom Mixer Strips (exact .mixer strips from ref3!)
            HStack(spacing: 12) {
                let micOpen = engine.isIntercomActive && (engine.pttActive || engine.latchActive)
                let ifbOpen = engine.isIntercomActive && engine.ifbEnabled
                mixerStrip(
                    title: tr("Мікрофон", "Mic PTT"),
                    dbfs: engine.telemetry.micPeakDbfs,
                    level: $engine.micGain,
                    muted: !micOpen,
                    muteLabel: "PTT",
                    activeTint: s.red,
                    onToggleMute: { engine.togglePtt() },
                    extraLabel: "L",
                    extraActive: engine.isIntercomActive && engine.latchActive,
                    onToggleExtra: { engine.toggleLatch() }
                )

                mixerStrip(
                    title: tr("Режисер IFB", "Director IFB"),
                    dbfs: engine.telemetry.ifbPeakDbfs,
                    level: $engine.ifbVolume,
                    muted: !ifbOpen,
                    muteLabel: "M",
                    activeTint: s.accent,
                    onToggleMute: { engine.toggleIfb() },
                    extraLabel: nil,
                    extraActive: false,
                    onToggleExtra: {}
                )

                // Luma Histogram Card beside mixer
                VStack(alignment: .leading, spacing: 6) {
                    Text(tr("Гістограма", "Histogram"))
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(s.text2)
                    Spacer(minLength: 2)
                    HStack(alignment: .bottom, spacing: 2) {
                        ForEach(0..<engine.telemetry.histogram.count, id: \.self) { i in
                            RoundedRectangle(cornerRadius: 1.5)
                                .fill(s.text2.opacity(0.85))
                                .frame(maxWidth: .infinity)
                                .frame(height: max(3, CGFloat(engine.telemetry.histogram[i]) * 78))
                        }
                    }
                    .frame(height: 82, alignment: .bottom)
                    Text("EV \(String(format: "%+.1f", engine.exposureBias)) · ISO \(Int(engine.isoValue))")
                        .font(.system(size: 9.5, design: .monospaced))
                        .foregroundColor(s.text3)
                        .lineLimit(1)
                }
                .padding(10)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))
            }
            .frame(height: 146)
        }
    }

    private var liveTelemetryGraph: some View {
        Canvas { ctx, size in
            let samples = engine.telemetry.history
            guard samples.count >= 2 else { return }

            // Horizontal hairline grid
            for frac in [0.25, 0.5, 0.75] {
                let y = size.height * frac
                var grid = Path()
                grid.move(to: CGPoint(x: 0, y: y))
                grid.addLine(to: CGPoint(x: size.width, y: y))
                ctx.stroke(grid, with: .color(s.border), lineWidth: 0.75)
            }

            let maxMbps = max(50.0, (samples.map(\.bitrateMbps).max() ?? 50.0) * 1.25)
            let maxMs = max(25.0, (samples.map(\.latencyMs).max() ?? 25.0) * 1.4)
            let stepX = size.width / CGFloat(samples.count - 1)

            // Traffic blue curve + soft area
            var brPath = Path()
            var brFill = Path()
            for (idx, sp) in samples.enumerated() {
                let x = CGFloat(idx) * stepX
                let y = size.height - CGFloat(min(1.0, sp.bitrateMbps / maxMbps)) * (size.height - 14) - 7
                if idx == 0 {
                    brPath.move(to: CGPoint(x: x, y: y))
                    brFill.move(to: CGPoint(x: x, y: size.height))
                    brFill.addLine(to: CGPoint(x: x, y: y))
                } else {
                    brPath.addLine(to: CGPoint(x: x, y: y))
                    brFill.addLine(to: CGPoint(x: x, y: y))
                }
            }
            brFill.addLine(to: CGPoint(x: size.width, y: size.height))
            brFill.closeSubpath()
            ctx.fill(brFill, with: .color(s.accent.opacity(0.12)))
            ctx.stroke(brPath, with: .color(s.accent), lineWidth: 1.6)

            // Latency violet curve
            var latPath = Path()
            for (idx, sp) in samples.enumerated() {
                let x = CGFloat(idx) * stepX
                let y = size.height - CGFloat(min(1.0, sp.latencyMs / maxMs)) * (size.height - 14) - 7
                if idx == 0 { latPath.move(to: CGPoint(x: x, y: y)) }
                else { latPath.addLine(to: CGPoint(x: x, y: y)) }
                if sp.dropout {
                    ctx.fill(Path(ellipseIn: CGRect(x: x - 3, y: y - 3, width: 6, height: 6)), with: .color(s.red))
                }
            }
            ctx.stroke(latPath, with: .color(s.violet), lineWidth: 1.4)
        }
    }

    private func diagPill(label: String, value: String, accent: Color) -> some View {
        HStack(spacing: 5) {
            Text("\(label):")
                .foregroundColor(s.text2)
            Text(value)
                .fontWeight(.semibold)
                .foregroundColor(accent)
        }
        .font(.system(size: 10.5).monospacedDigit())
        .padding(.horizontal, 9)
        .padding(.vertical, 4)
        .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.surface2))
    }

    private func mixerStrip(
        title: String,
        dbfs: Float,
        level: Binding<Float>,
        muted: Bool,
        muteLabel: String,
        activeTint: Color,
        onToggleMute: @escaping () -> Void,
        extraLabel: String?,
        extraActive: Bool,
        onToggleExtra: @escaping () -> Void
    ) -> some View {
        VStack(spacing: 6) {
            Text(title)
                .font(.system(size: 10.5, weight: .medium))
                .foregroundColor(s.text)
                .lineLimit(1)

            HStack(spacing: 8) {
                HStack(spacing: 2) {
                    StudioVUMeterView(dbfs: muted ? -60 : dbfs, height: 82)
                    StudioVUMeterView(dbfs: muted ? -60 : (dbfs - 2.5), height: 82)
                }
                .opacity(muted ? 0.35 : 1.0)

                // Interactive vertical fader track with glossy white cap (exact ref3 aesthetic!)
                GeometryReader { g in
                    let norm = CGFloat(max(0.0, min(1.0, level.wrappedValue / 1.5)))
                    let yPos = g.size.height * (1.0 - norm)
                    ZStack {
                        Capsule()
                            .fill(s.surface3)
                            .frame(width: 3)
                        RoundedRectangle(cornerRadius: 4, style: .continuous)
                            .fill(LinearGradient(colors: [.white, Color(hex: 0xE5E5EA)], startPoint: .top, endPoint: .bottom))
                            .frame(width: 22, height: 11)
                            .overlay(Rectangle().fill(Color.black.opacity(0.3)).frame(width: 12, height: 1))
                            .shadow(color: Color.black.opacity(0.35), radius: 2, y: 1)
                            .position(x: g.size.width / 2, y: max(6, min(g.size.height - 6, yPos)))
                    }
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { val in
                                let frac = 1.0 - max(0.0, min(1.0, val.location.y / max(1, g.size.height)))
                                level.wrappedValue = Float(frac * 1.5)
                            }
                    )
                }
                .frame(width: 24, height: 82)
            }

            HStack(spacing: 4) {
                Button(action: onToggleMute) {
                    Text(muteLabel)
                        .font(.system(size: 9.5, weight: .bold))
                        .foregroundColor(!muted ? .white : s.text2)
                        .frame(minWidth: 26, minHeight: 20)
                        .padding(.horizontal, 4)
                        .background(RoundedRectangle(cornerRadius: 5, style: .continuous).fill(!muted ? activeTint : s.surface2))
                }
                .buttonStyle(.plain)

                if let extraLabel {
                    Button(action: onToggleExtra) {
                        Text(extraLabel)
                            .font(.system(size: 9.5, weight: .bold))
                            .foregroundColor(extraActive ? .black : s.text2)
                            .frame(width: 22, height: 20)
                            .background(RoundedRectangle(cornerRadius: 5, style: .continuous).fill(extraActive ? s.yellow : s.surface2))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(width: 88)
        .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
        .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))
    }

    // MARK: - Drawer 4: Пристрої у мережі (1:1 port of ref1)

    private func devicesDrawerBody(isCompact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text(tr("У мережі", "On Network"))
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(s.text)
                    Text("\(engine.browser.studios.count) \(tr("студій", "studios")) · \(engine.browser.duoPeers.count) iPhone Duo")
                        .font(.system(size: 11))
                        .foregroundColor(s.text3)
                }
                Spacer()
                StudioIconButton(systemName: "arrow.clockwise", size: 28, bordered: true) {
                    engine.startDiscovery()
                }
                Button {
                    engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                } label: {
                    Text(tr("Демо-студія", "Demo Studio"))
                        .font(.system(size: 11.5, weight: .semibold))
                        .foregroundColor(s.primaryFg)
                        .padding(.horizontal, 10)
                        .frame(height: 28)
                        .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.primaryBg))
                }
                .buttonStyle(.plain)
            }

            // Discovered studios list (exact .list row style from ref1!)
            VStack(spacing: 0) {
                if engine.browser.studios.isEmpty {
                    HStack(spacing: 10) {
                        Image(systemName: "wifi")
                            .foregroundColor(s.text3)
                        Text(tr("Пошук LanVid Video / Studio у локальній Wi-Fi мережі…",
                                "Searching for LanVid Video / Studio on local Wi-Fi…"))
                            .font(.system(size: 12))
                            .foregroundColor(s.text3)
                        Spacer()
                    }
                    .padding(12)
                } else {
                    ForEach(Array(engine.browser.studios.enumerated()), id: \.element.id) { idx, st in
                        if idx > 0 { Divider().overlay(s.border) }
                        let selected = (engine.destIp == st.host && engine.videoPort == Int(st.videoPort))
                        Button {
                            engine.selectStudio(st)
                        } label: {
                            HStack(spacing: 10) {
                                ZStack {
                                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                                        .fill(selected ? s.accentSoft : s.surface2)
                                        .frame(width: 32, height: 32)
                                    Image(systemName: "desktopcomputer")
                                        .font(.system(size: 14))
                                        .foregroundColor(selected ? s.accent : s.text2)
                                }
                                VStack(alignment: .leading, spacing: 2) {
                                    HStack(spacing: 6) {
                                        Text(st.name)
                                            .font(.system(size: 13, weight: .medium))
                                            .foregroundColor(s.text)
                                            .lineLimit(1)
                                        StudioBadge(text: tr("Студія", "Studio"), tone: selected ? .accent : .neutral)
                                    }
                                    HStack(spacing: 5) {
                                        Circle().fill(s.green).frame(width: 6, height: 6)
                                        Text("\(st.host):\(st.videoPort) · Tally :\(st.tallyPort)")
                                            .font(.system(size: 11, design: .monospaced))
                                            .foregroundColor(s.text3)
                                    }
                                }
                                Spacer()
                                if selected {
                                    Image(systemName: "checkmark.circle.fill")
                                        .foregroundColor(s.accent)
                                }
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 9)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.surface))
            .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.border, lineWidth: 0.75))

            // Manual IP entry (like "За IP і портом" in ref1)
            HStack(spacing: 8) {
                TextField(tr("IP студії", "Studio IP"), text: $engine.destIp)
                    .font(.system(size: 12.5, design: .monospaced))
                    .foregroundColor(s.text)
                    .keyboardType(.decimalPad)
                    .padding(.horizontal, 10)
                    .frame(height: 32)
                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.surface))
                    .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.borderStrong, lineWidth: 0.75))

                Text(":\(engine.videoPort)")
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .foregroundColor(s.text2)
                    .padding(.horizontal, 9)
                    .frame(height: 32)
                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.surface2))

                Button { showDuoSheet = true } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "rectangle.split.2x1")
                        Text("Duo")
                    }
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(s.text)
                    .padding(.horizontal, 10)
                    .frame(height: 32)
                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.surface2))
                    .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).stroke(s.borderStrong, lineWidth: 0.75))
                }
                .buttonStyle(.plain)
            }
        }
    }

    // MARK: - Bottom LanVid Studio Dock (Stacked Icon+Label Segmented Tabs + Intercom + Primary GO LIVE Button)

    private func bottomStudioDock(isLandscape: Bool, isCompact: Bool) -> some View {
        HStack(spacing: 6) {
            // 4 LanVid Studio Segmented Tabs (stacked icon + full crisp label — zero truncation!)
            HStack(spacing: 2) {
                ForEach(StudioDrawerTab.allCases) { tab in
                    let isActive = (activeDrawer == tab)
                    Button {
                        withAnimation(.spring(response: 0.24, dampingFraction: 0.86)) {
                            showStabPopover = false
                            activeDrawer = isActive ? nil : tab
                        }
                    } label: {
                        Group {
                            if isLandscape {
                                HStack(spacing: 5) {
                                    Image(systemName: tab.icon)
                                        .font(.system(size: 12.5, weight: .medium))
                                    Text(tab.title)
                                        .font(.system(size: 12, weight: isActive ? .semibold : .medium))
                                        .lineLimit(1)
                                }
                            } else {
                                VStack(spacing: 2) {
                                    Image(systemName: tab.icon)
                                        .font(.system(size: 13.5, weight: isActive ? .semibold : .medium))
                                    Text(tab.title)
                                        .font(.system(size: 9.5, weight: isActive ? .semibold : .medium))
                                        .lineLimit(1)
                                        .minimumScaleFactor(0.8)
                                }
                            }
                        }
                        .foregroundColor(isActive ? s.text : s.text2)
                        .frame(maxWidth: .infinity)
                        .frame(height: isLandscape ? 34 : 40)
                        .padding(.horizontal, 3)
                        .background(
                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                .fill(isActive ? s.segActive : Color.clear)
                                .shadow(color: isActive ? Color.black.opacity(s.isDark ? 0.30 : 0.08) : .clear, radius: 2, y: 1)
                        )
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(3)
            .background(RoundedRectangle(cornerRadius: 11, style: .continuous).fill(s.surface2))
            .overlay(RoundedRectangle(cornerRadius: 11, style: .continuous).stroke(s.border, lineWidth: 0.75))

            // Independent Intercom ON/OFF + Quick Mic PTT pills (always visible, even in compact portrait!)
            let quickBtnW: CGFloat = (isCompact && !isLandscape) ? 32 : 34
            let micLive = engine.isIntercomActive && (engine.pttActive || engine.latchActive)

            Button {
                engine.toggleIntercom()
            } label: {
                ZStack(alignment: .topTrailing) {
                    Image(systemName: "headphones")
                        .font(.system(size: 13.5, weight: .semibold))
                        .foregroundColor(engine.isIntercomActive ? s.green : s.text3)
                        .frame(width: quickBtnW, height: isLandscape ? 40 : 46)
                    if engine.isIntercomActive {
                        Circle()
                            .fill(engine.directorSpeaking ? s.accent : s.green)
                            .frame(width: 6.5, height: 6.5)
                            .padding(5)
                    }
                }
                .background(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(engine.isIntercomActive ? s.greenSoft : s.surface2)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(engine.isIntercomActive ? (engine.directorSpeaking ? s.accent : s.green.opacity(0.65)) : s.border, lineWidth: 0.75)
                )
            }
            .buttonStyle(.plain)
            .accessibilityLabel(engine.isIntercomActive ? tr("Вимкнути інтерком", "Turn Intercom Off") : tr("Увімкнути інтерком", "Turn Intercom On"))

            Button {
                engine.togglePtt()
            } label: {
                Image(systemName: micLive ? "mic.fill" : "mic")
                    .font(.system(size: 13.5, weight: .semibold))
                    .foregroundColor(micLive ? .white : (engine.isIntercomActive ? s.text : s.text3))
                    .frame(width: quickBtnW, height: isLandscape ? 40 : 46)
                    .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(micLive ? s.red : s.surface2))
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(micLive ? s.red : s.border, lineWidth: 0.75))
            }
            .buttonStyle(.plain)
            .accessibilityLabel(micLive ? tr("Вимкнути мікрофон", "Mute Intercom Mic") : tr("Говорити в інтерком", "Intercom Mic PTT"))

            // Primary Action Button (.btn-primary white / .btn-danger.solid red when live)
            Button {
                engine.toggleStreaming()
            } label: {
                HStack(spacing: 5) {
                    if engine.isStreaming || engine.isDemoMode {
                        RoundedRectangle(cornerRadius: 2).fill(Color.white).frame(width: 8.5, height: 8.5)
                        Text(tr("Стоп", "Stop"))
                            .font(.system(size: 12, weight: .semibold))
                    } else {
                        Circle().fill(s.red).frame(width: 7.5, height: 7.5)
                        Text(tr("Ефір", "Live"))
                            .font(.system(size: 12, weight: .semibold))
                    }
                }
                .foregroundColor((engine.isStreaming || engine.isDemoMode) ? .white : s.primaryFg)
                .padding(.horizontal, 11)
                .frame(height: isLandscape ? 40 : 46)
                .background(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill((engine.isStreaming || engine.isDemoMode) ? s.red : s.primaryBg)
                )
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).fill(s.hudBar))
        .overlay(RoundedRectangle(cornerRadius: StudioPalette.rMd, style: .continuous).stroke(s.borderStrong, lineWidth: 0.75))
    }

    // MARK: - Mode 3: iPhone Duo Director Companion Desk

    private func duoCompanionDesk(isLandscape: Bool, safeTop: CGFloat, safeBot: CGFloat,
                                  safeLead: CGFloat, safeTrail: CGFloat) -> some View {
        ScrollView {
            VStack(spacing: 14) {
                HStack {
                    LanVidBrandMark(size: 26)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(tr("iPhone Duo — Пульт Режисера", "iPhone Duo — Director Companion"))
                            .font(.system(size: 17, weight: .semibold))
                            .foregroundColor(s.text)
                        Text(tr("Бездротовий контроль фокусу, експозиції, Tally та інтеркому",
                                "Wireless Focus Pull, Exposure, Tally & Duplex Intercom"))
                            .font(.system(size: 11.5))
                            .foregroundColor(s.text2)
                    }
                    Spacer()
                    Button(tr("Видошукач", "Viewfinder")) { engine.setDuoMode(.singleHUD) }
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(s.primaryFg)
                        .padding(.horizontal, 12)
                        .frame(height: 32)
                        .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(s.primaryBg))
                }

                // Tally Switcher
                StudioCard(padding: 12) {
                    HStack(spacing: 8) {
                        ForEach(ProTallyState.allCases, id: \.self) { st in
                            let active = (engine.tallyState == st)
                            let tint: Color = st == .program ? s.red : (st == .preview ? s.green : s.segActive)
                            Button {
                                engine.tallyState = st
                                engine.remoteRec = (st == .program)
                                engine.sendCompanionCommandToSelectedPeer()
                            } label: {
                                Text(st.badgeText)
                                    .font(.system(size: 12.5, weight: .semibold))
                                    .frame(maxWidth: .infinity)
                                    .frame(height: 40)
                                    .background(RoundedRectangle(cornerRadius: StudioPalette.rSm, style: .continuous).fill(active ? tint : s.surface2))
                                    .foregroundColor(active ? .white : s.text2)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }

                // Wireless Rack Focus Pull A <-> B
                StudioCard(padding: 14) {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text(tr("Бездротовий фокус (Rack Focus A ↔ B)", "Wireless Focus Pull (A ↔ B)"))
                                .font(.system(size: 13, weight: .semibold))
                                .foregroundColor(s.text)
                            Spacer()
                            Text("\(Int(engine.focusPosition * 100))%")
                                .font(.system(size: 13, weight: .bold, design: .monospaced))
                                .foregroundColor(s.accent)
                        }
                        Slider(value: $engine.focusPosition, in: 0...1)
                            .onChange(of: engine.focusPosition) { _ in
                                engine.autoFocus = false
                                engine.sendCompanionCommandToSelectedPeer()
                            }
                            .tint(s.accent)
                        HStack(spacing: 8) {
                            Button(tr("Точка A (25%)", "Mark A (25%)")) { engine.rackFocus(to: engine.focusPointA) }
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundColor(s.text)
                                .frame(maxWidth: .infinity, minHeight: 36)
                                .background(RoundedRectangle(cornerRadius: StudioPalette.rSm).fill(s.surface2))
                                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm).stroke(s.borderStrong, lineWidth: 0.75))
                            Button(tr("Точка B (78%)", "Mark B (78%)")) { engine.rackFocus(to: engine.focusPointB) }
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundColor(s.text)
                                .frame(maxWidth: .infinity, minHeight: 36)
                                .background(RoundedRectangle(cornerRadius: StudioPalette.rSm).fill(s.surface2))
                                .overlay(RoundedRectangle(cornerRadius: StudioPalette.rSm).stroke(s.borderStrong, lineWidth: 0.75))
                        }
                    }
                }

                StudioCard(padding: 14) { cameraDrawerBody(isCompact: false) }
                bottomStudioDock(isLandscape: isLandscape, isCompact: false)
            }
            .padding(.top, safeTop)
            .padding(.bottom, safeBot)
            .padding(.leading, safeLead)
            .padding(.trailing, safeTrail)
        }
        .background(s.bg.ignoresSafeArea())
    }

    // MARK: - Launch Arguments Support (Demo & App Store Screenshot Presets)

    private func applyLaunchScreenshotPresetIfNeeded() {
        let args = CommandLine.arguments
        if let idx = args.firstIndex(of: "-LVShot"), idx + 1 < args.count, let shot = Int(args[idx + 1]) {
            switch shot {
            case 1:
                // 1) Pro Viewfinder + Camera & Stabilization Deck
                engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                engine.stabilizationLevel = .cinematic
                engine.activeHardwareStabilization = .cinematic
                engine.touchHUD.normalizedPoint = CGPoint(x: 0.48, y: 0.36)
                engine.touchHUD.visible = true
                engine.exposureBias = 0.4
                activeDrawer = .camera
            case 2:
                // 2) Live Monitoring KPIs + Dual-Line Chart + VU Mixer Strips
                engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                engine.directorSpeaking = true
                engine.pttActive = true
                activeDrawer = .monitor
            case 3:
                // 3) Live Studio Tally (PGM • ON AIR) + Touch Reticle + Quick Zoom/Stabilization Strip
                engine.startDemoCineStudio(tally: .program, mode: .singleHUD)
                engine.remoteRec = true
                engine.directorSpeaking = true
                engine.zoomFactor = 2.0
                engine.lens = .tele
                engine.aeAfLocked = true
                engine.touchHUD.normalizedPoint = CGPoint(x: 0.50, y: 0.46)
                engine.touchHUD.visible = true
                engine.touchHUD.locked = true
                activeDrawer = nil
            case 4:
                // 4) Quality & Transmission Settings (matches ref4)
                engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
                engine.pttActive = true
                engine.latchActive = true
                engine.ifbEnabled = true
                activeDrawer = .quality
            default:
                break
            }
        } else if args.contains("-LVDemo") {
            engine.startDemoCineStudio(tally: .preview, mode: .singleHUD)
            activeDrawer = .camera
        }
    }
}

private struct FpsOption: Identifiable, Hashable {
    let value: Int
    var id: Int { value }
    init(_ value: Int) { self.value = value }
}
