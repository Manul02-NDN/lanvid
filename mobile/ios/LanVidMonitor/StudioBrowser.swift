import Darwin
import Foundation
import Network

/// Finds LanVid Studio instances on the LAN via BOTH:
///  1. Bonjour / mDNS ("_lanvid._udp"), and
///  2. UDP Broadcast Discovery ("LV_MON_DISC_REQ" -> UDP :5021 on 255.255.255.255 + every IPv4 subnet broadcast),
/// so Windows PCs and routers where mDNS fails are still discovered instantly.
@MainActor
final class StudioBrowser: ObservableObject {
    @Published private(set) var studios: [StudioEndpoint] = []
    @Published private(set) var searching = false
    @Published private(set) var status = ""

    private var browser: NWBrowser?
    private var resolving: [String: NWConnection] = [:]
    private var bcastTask: Task<Void, Never>?

    func start() {
        stop()
        if CommandLine.arguments.contains("-LVFakeStudios") {
            studios = [StudioEndpoint(name: "Studio PC — LanVid Studio", host: "192.168.1.20", port: 5020),
                       StudioEndpoint(name: "MacBook Pro — LanVid Studio", host: "192.168.1.34", port: 5020)]
            status = ""
            return
        }
        if CommandLine.arguments.contains("-LVDemo") { return }

        let params = NWParameters.udp
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_lanvid._udp", domain: nil), using: params)
        b.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                switch state {
                case .ready:
                    self?.status = tr("Пошук LanVid Studio у Wi-Fi мережі (mDNS + LAN Broadcast)…",
                                      "Searching for LanVid Studio on Wi-Fi (mDNS + LAN Broadcast)…")
                case .failed(let e):
                    self?.status = tr("mDNS недоступний (", "mDNS unavailable (") + e.localizedDescription + ") — LAN Broadcast"
                case .waiting(let e):
                    self?.status = tr("Дозвольте доступ до локальної мережі", "Allow Local Network access") + " (\(e.localizedDescription))"
                default: break
                }
            }
        }
        b.browseResultsChangedHandler = { [weak self] results, _ in
            Task { @MainActor in self?.handle(results) }
        }
        browser = b
        searching = true
        b.start(queue: .main)

        bcastTask = Task.detached(priority: .utility) { [weak self] in
            await self?.runUdpBroadcastLoop()
        }
    }

    func stop() {
        bcastTask?.cancel()
        bcastTask = nil
        browser?.cancel()
        browser = nil
        for c in resolving.values { c.cancel() }
        resolving.removeAll()
        searching = false
    }

    private func addOrUpdate(_ ep: StudioEndpoint) {
        if let idx = studios.firstIndex(where: { $0.host == ep.host && $0.port == ep.port }) {
            studios[idx] = ep
        } else {
            studios.append(ep)
        }
    }

    private func handle(_ results: Set<NWBrowser.Result>) {
        for r in results {
            guard case let .service(name, _, _, _) = r.endpoint else { continue }
            if studios.contains(where: { $0.name == name }) || resolving[name] != nil { continue }
            resolve(name: name, endpoint: r.endpoint)
        }
    }

    private func resolve(name: String, endpoint: NWEndpoint) {
        let params = NWParameters.udp
        if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ip.version = .v4
        }
        let c = NWConnection(to: endpoint, using: params)
        resolving[name] = c
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard case .ready = state, let c else { return }
            if case let .hostPort(host, port) = c.currentPath?.remoteEndpoint {
                var h = "\(host)"
                if let pct = h.firstIndex(of: "%") { h = String(h[..<pct]) }
                let ep = StudioEndpoint(name: name, host: h, port: port.rawValue)
                Task { @MainActor in
                    guard let self else { return }
                    self.addOrUpdate(ep)
                    self.resolving[name]?.cancel()
                    self.resolving[name] = nil
                }
            }
        }
        c.start(queue: .main)
    }

    nonisolated private func runUdpBroadcastLoop() async {
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return }
        defer { Darwin.close(fd) }

        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, socklen_t(MemoryLayout<Int32>.size))
        var tv = timeval(tv_sec: 0, tv_usec: 350_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        let req = Array("LV_MON_DISC_REQ".utf8)
        let prefix = "LV_MON_DISC_RESP|"
        var buf = [UInt8](repeating: 0, count: 512)
        var lastSend = Date.distantPast

        while !Task.isCancelled {
            let now = Date()
            if now.timeIntervalSince(lastSend) >= 1.2 {
                lastSend = now
                for ip in Self.broadcastAddresses() {
                    var dst = sockaddr_in()
                    dst.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                    dst.sin_family = sa_family_t(AF_INET)
                    dst.sin_port = UInt16(5021).bigEndian
                    inet_pton(AF_INET, ip, &dst.sin_addr)
                    withUnsafePointer(to: &dst) { ptr in
                        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                            _ = req.withUnsafeBytes { raw in
                                Darwin.sendto(fd, raw.baseAddress, raw.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                            }
                        }
                    }
                }
            }

            var src = sockaddr_in()
            var srcLen = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &src) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.recvfrom(fd, &buf, buf.count, 0, sa, &srcLen)
                }
            }
            if n > prefix.utf8.count, let str = String(bytes: buf[0..<n], encoding: .utf8), str.hasPrefix(prefix) {
                let parts = str.dropFirst(prefix.count).split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false)
                let port = parts.first.flatMap { UInt16($0) } ?? 5020
                let name = parts.count >= 3 && !parts[2].isEmpty ? String(parts[2]) : "LanVid Studio"
                var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                var addr = src.sin_addr
                inet_ntop(AF_INET, &addr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
                let host = String(cString: ipBuf)
                let ep = StudioEndpoint(name: name, host: host, port: port)
                await MainActor.run { [weak self] in self?.addOrUpdate(ep) }
            }
        }
    }

    nonisolated private static func broadcastAddresses() -> [String] {
        var result: [String] = ["255.255.255.255"]
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return result }
        defer { freeifaddrs(ifaddr) }
        var cur: UnsafeMutablePointer<ifaddrs>? = first
        while let iface = cur {
            let flags = Int32(iface.pointee.ifa_flags)
            if (flags & IFF_UP) != 0 && (flags & IFF_LOOPBACK) == 0 && (flags & IFF_BROADCAST) != 0,
               let b = iface.pointee.ifa_dstaddr, b.pointee.sa_family == sa_family_t(AF_INET) {
                var sin = b.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
                var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &sin.sin_addr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
                let s = String(cString: ipBuf)
                if !s.isEmpty && !result.contains(s) { result.append(s) }
            }
            cur = iface.pointee.ifa_next
        }
        return result
    }
}
