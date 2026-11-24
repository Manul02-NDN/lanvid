import Darwin
import Foundation
import Network

/// Finds LanVid Studio instances on the LAN via BOTH:
///  1. Bonjour / mDNS ("_lanvid._udp"), and
///  2. a fast unicast UDP sweep of the local IPv4 /24 ("LV_MON_DISC_REQ" -> UDP :5021 on a.b.c.1 ... a.b.c.254),
/// so Windows PCs and routers where mDNS fails are still found.
///
/// The sweep deliberately uses plain unicast `sendto`: iOS allows it as soon as the user has approved
/// "Local Network", whereas broadcast / multicast traffic needs the special
/// com.apple.developer.networking.multicast entitlement (and SO_BROADCAST sockets fail with EPERM without it).
@MainActor
final class StudioBrowser: ObservableObject {
    @Published private(set) var studios: [StudioEndpoint] = []
    @Published private(set) var searching = false
    @Published private(set) var status = ""

    private var browser: NWBrowser?
    private var resolving: [String: NWConnection] = [:]
    private var scanner: SubnetScanner?

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
                    self?.status = tr("Пошук LanVid Studio у Wi-Fi мережі (mDNS + сканування LAN)…",
                                      "Searching for LanVid Studio on Wi-Fi (mDNS + LAN scan)…")
                case .failed(let e):
                    self?.status = tr("mDNS недоступний (", "mDNS unavailable (") + e.localizedDescription + tr(") — сканування LAN", ") — LAN scan")
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

        // Unicast sweep of the local /24; the last connected studio is probed first.
        let s = SubnetScanner { [weak self] ep in
            Task { @MainActor in self?.addOrUpdate(ep) }
        }
        scanner = s
        s.start(priorityHost: UserDefaults.standard.string(forKey: "lastHost"))
    }

    func stop() {
        scanner?.cancel()
        scanner = nil
        browser?.cancel()
        browser = nil
        for c in resolving.values { c.cancel() }
        resolving.removeAll()
        searching = false
    }

    private func addOrUpdate(_ ep: StudioEndpoint) {
        if let idx = studios.firstIndex(where: { $0.host == ep.host && $0.port == ep.port }) {
            if studios[idx].name != ep.name { studios[idx] = ep }
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
}

/// Fast unicast discovery of the local IPv4 /24 on UDP :5021.
///
/// One non-blocking UDP socket (no SO_BROADCAST). Each round sends `LV_MON_DISC_REQ` to the saved studio IP first
/// and then to every host a.b.c.1 ... a.b.c.254 except this phone (`usleep(400)` between packets so the iOS socket
/// buffer is never overrun: ~100-150 ms for 253 hosts) while reading `LV_MON_DISC_RESP|<port>|<channels>|<name>`
/// replies on the same socket for 600 ms from the start of the round. Rounds repeat every ~1.5 s until cancelled,
/// so a studio that is switched on later is found too.
private final class SubnetScanner: @unchecked Sendable {
    static let discoveryPort: UInt16 = 5021
    private static let request = Array("LV_MON_DISC_REQ".utf8)
    private static let responsePrefix = "LV_MON_DISC_RESP|"

    private let queue = DispatchQueue(label: "lanvid.studio.subnet-sweep", qos: .userInitiated)
    private let lock = NSLock()
    private var cancelled = false
    private let onFound: @Sendable (StudioEndpoint) -> Void

    init(onFound: @escaping @Sendable (StudioEndpoint) -> Void) { self.onFound = onFound }

    private var isCancelled: Bool {
        lock.lock(); defer { lock.unlock() }
        return cancelled
    }

    func cancel() {
        lock.lock(); cancelled = true; lock.unlock()
    }

    func start(priorityHost: String?) {
        queue.async { [self] in run(priorityHost: priorityHost) }
    }

    // MARK: Loop

    private func run(priorityHost: String?) {
        let fd = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return }
        defer { Darwin.close(fd) }
        let flags = fcntl(fd, F_GETFL, 0)
        _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        var sndbuf: Int32 = 1 << 20
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        var nosigpipe: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, socklen_t(MemoryLayout<Int32>.size))

        while !isCancelled {
            let roundStart = Date()
            let own = Self.localIPv4()
            var targets: [UInt32] = []
            if let p = priorityHost, let ip = Self.parse(p), ip != own { targets.append(ip) }
            if let own {
                let base = own & 0xFFFF_FF00
                for host in UInt32(1)...254 {
                    let ip = base | host
                    if ip != own, !targets.contains(ip) { targets.append(ip) }
                }
            }

            var sent = 0
            for ip in targets {
                if isCancelled { return }
                Self.send(fd, to: ip)
                sent += 1
                usleep(400)
                if sent % 16 == 0 { drain(fd) }  // keep reading replies during the sweep
            }
            listen(fd, until: roundStart.addingTimeInterval(0.6))
            listen(fd, until: Date().addingTimeInterval(0.9))  // pause between rounds, still reading
        }
    }

    /// Reads replies until `deadline` (poll in short slices so cancel() is honoured quickly).
    private func listen(_ fd: Int32, until deadline: Date) {
        while !isCancelled {
            let remaining = deadline.timeIntervalSinceNow
            if remaining <= 0 { return }
            var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let ms = Int32(min(remaining, 0.05) * 1000)
            if poll(&pfd, 1, max(ms, 1)) > 0 { drain(fd) }
        }
    }

    private func drain(_ fd: Int32) {
        var buf = [UInt8](repeating: 0, count: 512)
        while true {
            var src = sockaddr_in()
            var srcLen = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &src) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.recvfrom(fd, &buf, buf.count, 0, sa, &srcLen)
                }
            }
            if n <= 0 { return }  // EWOULDBLOCK: nothing more queued
            guard n > Self.responsePrefix.utf8.count,
                  let text = String(bytes: buf[0..<n], encoding: .utf8), text.hasPrefix(Self.responsePrefix) else { continue }
            let parts = text.dropFirst(Self.responsePrefix.count)
                .split(separator: "|", maxSplits: 2, omittingEmptySubsequences: false)
            let port = parts.first.flatMap { UInt16($0) } ?? 5020
            let name = parts.count >= 3 && !parts[2].isEmpty ? String(parts[2]) : "LanVid Studio"
            let host = Self.string(UInt32(bigEndian: src.sin_addr.s_addr))
            onFound(StudioEndpoint(name: name, host: host, port: port))
        }
    }

    // MARK: Helpers

    private static func send(_ fd: Int32, to ip: UInt32) {
        var dst = sockaddr_in()
        dst.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        dst.sin_family = sa_family_t(AF_INET)
        dst.sin_port = discoveryPort.bigEndian
        dst.sin_addr.s_addr = ip.bigEndian
        withUnsafePointer(to: &dst) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                for attempt in 0..<2 {
                    let r = request.withUnsafeBytes {
                        Darwin.sendto(fd, $0.baseAddress, $0.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                    // Socket buffer momentarily full: give it a moment and retry once. Other errors
                    // (EHOSTUNREACH, EPERM before the Local Network approval) are simply skipped.
                    if r < 0 && errno == ENOBUFS && attempt == 0 { usleep(2000); continue }
                    break
                }
            }
        }
    }

    /// IPv4 address (host byte order) of the active Wi-Fi interface en0; ignores loopback and link-local
    /// 169.254.x.x. Falls back to another en* / bridge* interface (e.g. USB-Ethernet, Personal Hotspot host).
    static func localIPv4() -> UInt32? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }
        var wifi: UInt32?
        var other: UInt32?
        var cur: UnsafeMutablePointer<ifaddrs>? = first
        while let ifa = cur {
            defer { cur = ifa.pointee.ifa_next }
            let flags = Int32(ifa.pointee.ifa_flags)
            guard (flags & IFF_UP) != 0, (flags & IFF_LOOPBACK) == 0, let sa = ifa.pointee.ifa_addr,
                  sa.pointee.sa_family == sa_family_t(AF_INET) else { continue }
            let ip = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
            if (ip >> 24) == 127 || (ip >> 16) == 0xA9FE || ip == 0 { continue }
            let name = String(cString: ifa.pointee.ifa_name)
            if name == "en0" { wifi = ip }
            else if other == nil, name.hasPrefix("en") || name.hasPrefix("bridge") { other = ip }
        }
        return wifi ?? other
    }

    private static func parse(_ s: String) -> UInt32? {
        var a = in_addr()
        guard inet_pton(AF_INET, s, &a) == 1 else { return nil }
        return UInt32(bigEndian: a.s_addr)
    }

    private static func string(_ ip: UInt32) -> String {
        "\(ip >> 24).\((ip >> 16) & 255).\((ip >> 8) & 255).\(ip & 255)"
    }
}
