package com.lanvid.monitor

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Finds LanVid Studio on the LAN through BOTH:
 *  1. mDNS / DNS-SD ("_lanvid._udp"), and
 *  2. UDP Broadcast Discovery ("LV_MON_DISC_REQ" -> UDP :5021 on 255.255.255.255 + every IPv4 subnet broadcast),
 * so Windows PCs and routers that block mDNS/Bonjour are still discovered instantly.
 */
class StudioDiscovery(context: Context) {
    val studios = mutableStateListOf<Studio>()
    var status by mutableStateOf("")
        private set

    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private var lock: WifiManager.MulticastLock? = null
    private val main = Handler(Looper.getMainLooper())
    private var listener: NsdManager.DiscoveryListener? = null
    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolving = false
    private val bcastRunning = AtomicBoolean(false)
    private var bcastThread: Thread? = null

    fun start() {
        stop()
        status = "Пошук LanVid Studio у Wi-Fi мережі (mDNS + LAN Broadcast)…"
        lock = wifi.createMulticastLock("lanvid-monitor-mdns").apply { setReferenceCounted(false); acquire() }

        // 1. Start mDNS / NSD discovery
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {
                main.post { status = "Пошук LanVid Studio у Wi-Fi мережі (mDNS + LAN Broadcast)…" }
            }
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                main.post { status = "mDNS недоступний (код $errorCode) — активний LAN Broadcast…" }
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                main.post { enqueue(info) }
            }
            override fun onServiceLost(info: NsdServiceInfo) {
                main.post { studios.removeAll { it.name == info.serviceName } }
            }
        }
        listener = l
        runCatching { nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l) }

        // 2. Start parallel UDP Broadcast Discovery (port 5021)
        bcastRunning.set(true)
        bcastThread = Thread({ runBroadcastLoop() }, "LanVid-UDP-Discovery").apply {
            isDaemon = true
            start()
        }
    }

    fun stop() {
        bcastRunning.set(false)
        bcastThread?.interrupt()
        bcastThread = null
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        listener = null
        pending.clear()
        resolving = false
        lock?.let { if (it.isHeld) it.release() }
        lock = null
    }

    private fun runBroadcastLoop() {
        var sock: DatagramSocket? = null
        try {
            sock = DatagramSocket().apply {
                broadcast = true
                soTimeout = 350
            }
            val reqBytes = DISC_REQ.toByteArray(Charsets.UTF_8)
            val rxBuf = ByteArray(512)
            var lastSendMs = 0L

            while (bcastRunning.get()) {
                val now = System.currentTimeMillis()
                if (now - lastSendMs >= 1200L) {
                    lastSendMs = now
                    val targets = collectBroadcastTargets()
                    for (addr in targets) {
                        runCatching {
                            sock.send(DatagramPacket(reqBytes, reqBytes.size, addr, DISC_PORT))
                        }
                    }
                }
                try {
                    val pkt = DatagramPacket(rxBuf, rxBuf.size)
                    sock.receive(pkt)
                    val text = String(pkt.data, pkt.offset, pkt.length, Charsets.UTF_8)
                    if (text.startsWith(DISC_RESP_PREFIX)) {
                        val parts = text.removePrefix(DISC_RESP_PREFIX).split("|", limit = 3)
                        val port = parts.getOrNull(0)?.toIntOrNull() ?: 5020
                        val name = parts.getOrNull(2)?.trim()?.ifBlank { "LanVid Studio" } ?: "LanVid Studio"
                        val host = pkt.address?.hostAddress?.substringBefore('%') ?: continue
                        main.post { addOrUpdateStudio(Studio(name, host, port)) }
                    }
                } catch (_: SocketTimeoutException) {
                    // Normal poll tick
                }
            }
        } catch (_: Exception) {
            // Socket closed on stop()
        } finally {
            runCatching { sock?.close() }
        }
    }

    private fun collectBroadcastTargets(): List<InetAddress> {
        val out = LinkedHashSet<InetAddress>()
        runCatching { out.add(InetAddress.getByName("255.255.255.255")) }
        runCatching {
            val ifaces = NetworkInterface.getNetworkInterfaces()
            while (ifaces != null && ifaces.hasMoreElements()) {
                val ni = ifaces.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    val b = ia.broadcast
                    if (b is Inet4Address) out.add(b)
                }
            }
        }
        return out.toList()
    }

    private fun addOrUpdateStudio(studio: Studio) {
        val idx = studios.indexOfFirst { it.host == studio.host && it.port == studio.port }
        if (idx >= 0) {
            if (studios[idx] != studio) studios[idx] = studio
        } else {
            studios.add(studio)
        }
    }

    private fun enqueue(info: NsdServiceInfo) {
        if (studios.any { it.name == info.serviceName } || pending.any { it.serviceName == info.serviceName }) return
        pending.addLast(info)
        resolveNext()
    }

    @Suppress("DEPRECATION")
    private fun resolveNext() {
        if (resolving) return
        val info = pending.removeFirstOrNull() ?: return
        resolving = true
        nsd.resolveService(info, object : NsdManager.ResolveListener {
            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                main.post { resolving = false; resolveNext() }
            }
            override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                val addr = serviceInfo.host
                val host = if (addr is Inet4Address) addr.hostAddress else addr?.hostAddress?.substringBefore('%')
                main.post {
                    if (host != null) {
                        addOrUpdateStudio(Studio(serviceInfo.serviceName, host, serviceInfo.port))
                    }
                    resolving = false
                    resolveNext()
                }
            }
        })
    }

    companion object {
        const val SERVICE_TYPE = "_lanvid._udp"
        const val DISC_PORT = 5021
        const val DISC_REQ = "LV_MON_DISC_REQ"
        const val DISC_RESP_PREFIX = "LV_MON_DISC_RESP|"
    }
}
