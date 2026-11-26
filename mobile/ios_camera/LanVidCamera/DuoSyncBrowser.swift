import Darwin
import Foundation
import Network
import UIKit

/// Discovers BOTH:
/// 1) Desktop LanVid Video / Studio receivers (mDNS `_lanvid._udp` + unicast `/24` sweep on UDP :5023), and
/// 2) Paired iPhone Duo `LanVid Camera` peers on the local Wi-Fi (UDP :5028), exchanging synchronized timecode,
///    shared CAM A / CAM B Tally state, and remote Director Companion focus/exposure commands.
@MainActor
final class DuoSyncBrowser: ObservableObject {
    static let videoDiscoveryPort: UInt16 = 5023
    static let duoSyncPort: UInt16 = 5028

    @Published private(set) var studios: [DiscoveredStudio] = []
    @Published private(set) var duoPeers: [DuoPeerCamera] = []
    @Published private(set) var searching = false
    @Published private(set) var status = ""

    var onRemoteDuoCommand: ((_ tally: ProTallyState?, _ focus: Float?, _ iso: Float?, _ shutter: Int?, _ wb: Float?, _ timecode: String?) -> Void)?

    private var browser: NWBrowser?
    private var resolving: [String: NWConnection] = [:]
    private var listener: NWListener?
    private var sweepTask: Task<Void, Never>?
    private var localRole: DuoRole = .camA
    private var localLabel: String = "CAM A"

    func start(localRole: DuoRole, localLabel: String, demoMode: Bool) {
        stop()
        self.localRole = localRole
        self.localLabel = localLabel

        if demoMode || CommandLine.arguments.contains("-LVDemo") || CommandLine.arguments.contains("-LVFakePeers") {
            studios = [
                DiscoveredStudio(name: "LanVid Video — Mac Studio", host: "192.168.1.24", videoPort: 5006, tallyPort: 5022),
                DiscoveredStudio(name: "LanVid Studio — Studio PC", host: "192.168.1.40", videoPort: 5006, tallyPort: 5022)
            ]
            duoPeers = [
                DuoPeerCamera(
                    host: "192.168.1.62",
                    name: "iPhone 16 Pro • CAM B",
                    role: .camB,
                    tally: .preview,
                    timecode: "01:14:22:08",
                    focusPosition: 0.42,
                    isoValue: 400,
                    shutterAngle: 180,
                    wbKelvin: 5600,
                    lastSeen: Date()
                )
            ]
            status = tr("Demo Cine Studio • 2 студії та 1 iPhone Duo", "Demo Cine Studio • 2 studios & 1 iPhone Duo")
            return
        }

        startBonjourBrowser()
        startDuoListener()
        startSubnetSweep()
    }

    func stop() {
        sweepTask?.cancel()
        sweepTask = nil
        browser?.cancel()
        browser = nil
        listener?.cancel()
        listener = nil
        for c in resolving.values { c.cancel() }
        resolving.removeAll()
        searching = false
    }

    /// Sends a lightweight Duo Sync state or Director Companion control packet to a paired iPhone over unicast UDP.
    func sendDuoPacket(to host: String, role: DuoRole, label: String, tally: ProTallyState,
                       timecode: String, focus: Float, iso: Float, shutter: Int, wb: Float, isCommand: Bool) {
        let kind = isCommand ? "CMD" : "SYNC"
        let msg = "LV_DUO|\(kind)|\(role.rawValue)|\(label)|\(tally.rawValue)|\(timecode)|\(String(format: "%.3f", focus))|\(Int(iso))|\(shutter)|\(Int(wb))"
        guard let data = msg.data(using: .utf8) else { return }
        let conn = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: Self.duoSyncPort)!, using: .udp)
        conn.stateUpdateHandler = { state in
            if case .ready = state {
                conn.send(content: data, completion: .contentProcessed { _ in conn.cancel() })
            } else if case .failed = state {
                conn.cancel()
            }
        }
        conn.start(queue: .global(qos: .userInitiated))
    }

    // MARK: - Bonjour (_lanvid._udp)

    private func startBonjourBrowser() {
        let params = NWParameters.udp
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_lanvid._udp", domain: nil), using: params)
        b.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                switch state {
                case .ready:
                    self?.status = tr("Пошук LanVid Video та iPhone Duo у Wi-Fi…", "Searching for LanVid Video & iPhone Duo on Wi-Fi…")
                case .waiting(let e):
                    self?.status = tr("Дозвольте локальну мережу: ", "Allow Local Network: ") + e.localizedDescription
                case .failed(let e):
                    self?.status = tr("mDNS недоступний — сканування LAN (", "mDNS unavailable — LAN scan (") + e.localizedDescription + ")"
                default:
                    break
                }
            }
        }
        b.browseResultsChangedHandler = { [weak self] results, _ in
            Task { @MainActor in self?.handleBonjour(results) }
        }
        browser = b
        searching = true
        b.start(queue: .main)
    }

    private func handleBonjour(_ results: Set<NWBrowser.Result>) {
        for r in results {
            guard case let .service(name, _, _, _) = r.endpoint else { continue }
            if resolving[name] != nil { continue }
            var roleTag = "video"
            if case let .bonjour(txt) = r.metadata, let rv = txt["role"] {
                roleTag = rv
            }
            if roleTag == "audio" { continue }

            let params = NWParameters.udp
            if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
                ip.version = .v4
            }
            let c = NWConnection(to: r.endpoint, using: params)
            resolving[name] = c
            c.stateUpdateHandler = { [weak self, weak c] state in
                guard case .ready = state, let c else { return }
                if case let .hostPort(host, port) = c.currentPath?.remoteEndpoint {
                    var h = "\(host)"
                    if let pct = h.firstIndex(of: "%") { h = String(h[..<pct]) }
                    let p = port.rawValue
                    Task { @MainActor in
                        guard let self else { return }
                        if roleTag == "camera" || roleTag == "duo" {
                            self.upsertDuoPeer(host: h, name: name, role: .camB, tally: .standby,
                                               timecode: "01:00:00:00", focus: 0.5, iso: 400, shutter: 180, wb: 5600)
                        } else {
                            let ep = DiscoveredStudio(name: name, host: h, videoPort: p == 5022 ? 5006 : p, tallyPort: 5022)
                            if !self.studios.contains(where: { $0.host == ep.host }) {
                                self.studios.append(ep)
                            }
                        }
                        self.resolving[name]?.cancel()
                        self.resolving[name] = nil
                    }
                }
            }
            c.start(queue: .main)
        }
    }

    // MARK: - iPhone Duo UDP Sync Listener (:5028)

    private func startDuoListener() {
        do {
            let params = NWParameters.udp
            params.allowLocalEndpointReuse = true
            let l = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.duoSyncPort)!)
            l.service = NWListener.Service(
                name: "\(localLabel) (\(UIDevice.current.name))",
                type: "_lanvid._udp",
                txtRecord: NWTXTRecord(["role": "camera", "duo": localRole.rawValue])
            )
            l.newConnectionHandler = { [weak self] conn in
                conn.start(queue: .main)
                conn.receiveMessage { data, _, _, _ in
                    if let data, let text = String(data: data, encoding: .utf8) {
                        var remoteHost = ""
                        if case let .hostPort(h, _) = conn.endpoint {
                            remoteHost = "\(h)"
                            if let pct = remoteHost.firstIndex(of: "%") { remoteHost = String(remoteHost[..<pct]) }
                        }
                        let finalHost = remoteHost
                        Task { @MainActor [weak self] in self?.handleDuoMessage(text, from: finalHost) }
                    }
                    conn.cancel()
                }
            }
            l.start(queue: .main)
            listener = l
        } catch {
            // Port busy or simulator restriction — non-fatal
        }
    }

    private func handleDuoMessage(_ text: String, from host: String) {
        let parts = text.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 10, parts[0] == "LV_DUO" else { return }
        let kind = parts[1]
        let role = DuoRole(rawValue: parts[2]) ?? .camB
        let label = parts[3]
        let tally = ProTallyState(rawValue: Int(parts[4]) ?? 0) ?? .standby
        let tc = parts[5]
        let focus = Float(parts[6]) ?? 0.5
        let iso = Float(parts[7]) ?? 400
        let shutter = Int(parts[8]) ?? 180
        let wb = Float(parts[9]) ?? 5600

        if !host.isEmpty {
            upsertDuoPeer(host: host, name: label, role: role, tally: tally,
                          timecode: tc, focus: focus, iso: iso, shutter: shutter, wb: wb)
        }
        if kind == "CMD" {
            onRemoteDuoCommand?(tally, focus, iso, shutter, wb, tc)
        } else {
            onRemoteDuoCommand?(nil, nil, nil, nil, nil, tc)
        }
    }

    private func upsertDuoPeer(host: String, name: String, role: DuoRole, tally: ProTallyState,
                               timecode: String, focus: Float, iso: Float, shutter: Int, wb: Float) {
        let peer = DuoPeerCamera(host: host, name: name, role: role, tally: tally, timecode: timecode,
                                 focusPosition: focus, isoValue: iso, shutterAngle: shutter, wbKelvin: wb, lastSeen: Date())
        if let idx = duoPeers.firstIndex(where: { $0.host == host }) {
            duoPeers[idx] = peer
        } else {
            duoPeers.append(peer)
        }
    }

    // MARK: - Unicast /24 Sweep (Zero Broadcast Entitlement)

    private func startSubnetSweep() {
        let myRole = localRole
        let myLabel = localLabel
        sweepTask = Task.detached(priority: .utility) { [weak self] in
            guard let localIp = Self.primaryIPv4Address() else { return }
            let octets = localIp.split(separator: ".").compactMap { Int($0) }
            guard octets.count == 4 else { return }
            let prefix = "\(octets[0]).\(octets[1]).\(octets[2])."
            let selfHost = octets[3]

            let sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard sock >= 0 else { return }
            defer { close(sock) }
            let curFlags = fcntl(sock, F_GETFL, 0)
            _ = fcntl(sock, F_SETFL, curFlags | O_NONBLOCK)

            let probe = Array("LANVID_DISCOVER_V2|video".utf8)
            let duoPing = Array("LV_DUO|SYNC|\(myRole.rawValue)|\(myLabel)|0|01:00:00:00|0.500|400|180|5600".utf8)

            while !Task.isCancelled {
                for h in 1...254 where h != selfHost {
                    if Task.isCancelled { break }
                    let ipStr = "\(prefix)\(h)"
                    var addr = sockaddr_in()
                    addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                    addr.sin_family = sa_family_t(AF_INET)
                    inet_pton(AF_INET, ipStr, &addr.sin_addr)

                    addr.sin_port = Self.videoDiscoveryPort.bigEndian
                    _ = probe.withUnsafeBytes { buf in
                        withUnsafePointer(to: &addr) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                                sendto(sock, buf.baseAddress, buf.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                            }
                        }
                    }

                    addr.sin_port = Self.duoSyncPort.bigEndian
                    _ = duoPing.withUnsafeBytes { buf in
                        withUnsafePointer(to: &addr) {
                            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                                sendto(sock, buf.baseAddress, buf.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                            }
                        }
                    }
                    usleep(450)
                }

                // Collect replies for 600 ms
                var rxBuf = [UInt8](repeating: 0, count: 512)
                for _ in 0..<24 {
                    if Task.isCancelled { break }
                    var from = sockaddr_in()
                    var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)
                    let n = withUnsafeMutablePointer(to: &from) {
                        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                            recvfrom(sock, &rxBuf, rxBuf.count, 0, sa, &fromLen)
                        }
                    }
                    if n > 0, let str = String(bytes: rxBuf[0..<n], encoding: .utf8) {
                        var ipC = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                        var sinAddr = from.sin_addr
                        inet_ntop(AF_INET, &sinAddr, &ipC, socklen_t(INET_ADDRSTRLEN))
                        let host = String(cString: ipC)
                        await self?.parseSweepReply(str, host: host)
                    } else {
                        try? await Task.sleep(nanoseconds: 25_000_000)
                    }
                }
                try? await Task.sleep(nanoseconds: 2_500_000_000)
            }
        }
    }

    private func parseSweepReply(_ text: String, host: String) {
        if text.hasPrefix("LV_DUO|") {
            handleDuoMessage(text, from: host)
            return
        }
        // LanVid Video responder format (contains port and name)
        let parts = text.split(separator: "|").map(String.init)
        if parts.count >= 3 {
            let port = UInt16(parts[1]) ?? 5006
            let name = parts.last ?? "LanVid Video"
            let ep = DiscoveredStudio(name: name, host: host, videoPort: port, tallyPort: 5022)
            if !studios.contains(where: { $0.host == host }) {
                studios.append(ep)
            }
        }
    }

    private nonisolated static func primaryIPv4Address() -> String? {
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return nil }
        defer { freeifaddrs(ifaddr) }
        var cur: UnsafeMutablePointer<ifaddrs>? = first
        while let ptr = cur {
            let iface = ptr.pointee
            if iface.ifa_addr?.pointee.sa_family == UInt8(AF_INET) {
                let name = String(cString: iface.ifa_name)
                if name == "en0" || name == "en1" || name == "en2" {
                    var addr = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    getnameinfo(iface.ifa_addr, socklen_t(iface.ifa_addr.pointee.sa_len),
                                &addr, socklen_t(addr.count), nil, 0, NI_NUMERICHOST)
                    let ip = String(cString: addr)
                    if !ip.hasPrefix("127.") && !ip.hasPrefix("169.254.") { return ip }
                }
            }
            cur = iface.ifa_next
        }
        return nil
    }
}
