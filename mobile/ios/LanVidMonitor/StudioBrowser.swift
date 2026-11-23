import Foundation
import Network

/// Finds LanVid Studio instances on the LAN: Bonjour / mDNS service "_lanvid._udp" (published by Studio
/// while its mobile monitor server runs). Each result is resolved to an IP + port with a short-lived
/// UDP NWConnection (the resolved path's remote endpoint), so connecting is one tap.
@MainActor
final class StudioBrowser: ObservableObject {
    @Published private(set) var studios: [StudioEndpoint] = []
    @Published private(set) var searching = false
    @Published private(set) var status = ""

    private var browser: NWBrowser?
    private var resolving: [String: NWConnection] = [:]

    func start() {
        stop()
        // Screenshot builds only (-LVFakeStudios): show two discovered studios instead of browsing.
        if CommandLine.arguments.contains("-LVFakeStudios") {
            studios = [StudioEndpoint(name: "Studio PC — LanVid Studio", host: "192.168.1.20", port: 5020),
                       StudioEndpoint(name: "MacBook Pro — LanVid Studio", host: "192.168.1.34", port: 5020)]
            status = ""
            return
        }
        if CommandLine.arguments.contains("-LVDemo") { return }  // no Local Network prompt over the demo run
        let params = NWParameters.udp
        params.includePeerToPeer = false
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_lanvid._udp", domain: nil), using: params)
        b.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                switch state {
                case .ready: self?.status = tr("Пошук LanVid Studio у Wi-Fi мережі…", "Searching for LanVid Studio on your Wi-Fi…")
                case .failed(let e): self?.status = tr("Пошук недоступний: ", "Search unavailable: ") + e.localizedDescription
                case .waiting(let e): self?.status = tr("Дозвольте доступ до локальної мережі", "Allow Local Network access") + " (\(e.localizedDescription))"
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
    }

    func stop() {
        browser?.cancel()
        browser = nil
        for c in resolving.values { c.cancel() }
        resolving.removeAll()
        searching = false
    }

    private func handle(_ results: Set<NWBrowser.Result>) {
        var alive = Set<String>()
        for r in results {
            guard case let .service(name, _, _, _) = r.endpoint else { continue }
            alive.insert(name)
            if studios.contains(where: { $0.name == name }) || resolving[name] != nil { continue }
            resolve(name: name, endpoint: r.endpoint)
        }
        studios.removeAll { !alive.contains($0.name) }
    }

    private func resolve(name: String, endpoint: NWEndpoint) {
        let params = NWParameters.udp
        if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ip.version = .v4  // Studio binds IPv4
        }
        let c = NWConnection(to: endpoint, using: params)
        resolving[name] = c
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard case .ready = state, let c else { return }
            if case let .hostPort(host, port) = c.currentPath?.remoteEndpoint {
                var h = "\(host)"
                if let pct = h.firstIndex(of: "%") { h = String(h[..<pct]) }  // strip interface scope
                let ep = StudioEndpoint(name: name, host: h, port: port.rawValue)
                Task { @MainActor in
                    guard let self else { return }
                    if !self.studios.contains(ep) { self.studios.append(ep) }
                    self.resolving[name]?.cancel()
                    self.resolving[name] = nil
                }
            }
        }
        c.start(queue: .main)
    }
}
