package com.lanvid.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.net.Inet4Address
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Bulletproof 3-layer LAN discovery for LanVid Camera:
 *   1. Acquires Android WifiManager.MulticastLock so Wi-Fi chips never drop broadcast/mDNS replies.
 *   2. Continuous mDNS / Bonjour (_lanvid._udp) via NsdManager (works even when routers block UDP broadcast).
 *   3. Continuous UDP Broadcast probe via LanVidNative.scanLanPeers (:5023 Video, :5021 Audio, :5099 Legacy).
 *
 * Results are merged, deduplicated (excluding loopback), and sorted with [VIDEO] studios first.
 */
class LanPeerDiscovery(context: Context) {
    private val appCtx = context.applicationContext
    private val nsd = appCtx.getSystemService(Context.NSD_SERVICE) as? NsdManager
    private val wifi = appCtx.getSystemService(Context.WIFI_SERVICE) as? WifiManager
    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor()

    private var lock: WifiManager.MulticastLock? = null
    private var listener: NsdManager.DiscoveryListener? = null
    private val running = AtomicBoolean(false)

    private val mdnsPeers = LinkedHashMap<String, LanPeer>()
    private val udpPeers = LinkedHashMap<String, LanPeer>()
    private val peersLock = Any()

    val peers = mutableStateListOf<LanPeer>()
    var scanning by mutableStateOf(false)
        private set

    fun start() {
        if (!running.compareAndSet(false, true)) {
            triggerUdpScan()
            return
        }
        acquireMulticastLock()
        startMdns()
        main.post(udpLoop)
    }

    fun refreshNow() {
        if (!running.get()) {
            start()
            return
        }
        triggerUdpScan()
    }

    fun stop() {
        if (!running.compareAndSet(true, false)) return
        main.removeCallbacks(udpLoop)
        stopMdns()
        releaseMulticastLock()
    }

    private val udpLoop = object : Runnable {
        override fun run() {
            if (!running.get()) return
            triggerUdpScan()
            main.postDelayed(this, 2500L)
        }
    }

    private fun triggerUdpScan() {
        if (!running.get()) return
        main.post { scanning = true }
        io.execute {
            val found = runCatching { LanVidNative.scanLanPeers(600) }.getOrDefault(emptyList())
            synchronized(peersLock) {
                udpPeers.clear()
                for (p in found) {
                    if (p.ip.isBlank() || p.ip == "127.0.0.1" || p.ip == "0.0.0.0") continue
                    udpPeers["${p.ip}:${p.port}"] = p
                }
            }
            publishMerged()
            main.post { scanning = false }
        }
    }

    private fun startMdns() {
        val mgr = nsd ?: return
        stopMdns()
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(regType: String) = Unit
            override fun onDiscoveryStopped(serviceType: String) = Unit
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit

            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                if (!running.get()) return
                resolveService(mgr, serviceInfo)
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) {
                synchronized(peersLock) {
                    mdnsPeers.values.removeAll { it.name == serviceInfo.serviceName }
                }
                publishMerged()
            }
        }
        listener = l
        runCatching { mgr.discoverServices("_lanvid._udp.", NsdManager.PROTOCOL_DNS_SD, l) }
    }

    private fun resolveService(mgr: NsdManager, info: NsdServiceInfo) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            runCatching {
                mgr.registerServiceInfoCallback(info, io, object : NsdManager.ServiceInfoCallback {
                    override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) = Unit
                    override fun onServiceUpdated(s: NsdServiceInfo) {
                        val ip = s.hostAddresses.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }?.hostAddress
                            ?: return
                        if (s.port <= 0) return
                        val role = s.attributes["role"]?.let { String(it, Charsets.UTF_8) } ?: ""
                        // Ignore pure audio-only monitor servers on port 5020 when advertising video targets
                        val peer = LanPeer(
                            ip = ip,
                            port = s.port,
                            name = s.serviceName ?: "LanVid PC",
                            os = "lan",
                            simd = "mDNS",
                            rttMs = 1,
                            role = role
                        )
                        synchronized(peersLock) {
                            mdnsPeers["${peer.ip}:${peer.port}"] = peer
                        }
                        publishMerged()
                        runCatching { mgr.unregisterServiceInfoCallback(this) }
                    }
                    override fun onServiceLost() = Unit
                    override fun onServiceInfoCallbackUnregistered() = Unit
                })
            }
        } else {
            @Suppress("DEPRECATION")
            runCatching {
                mgr.resolveService(info, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
                    override fun onServiceResolved(s: NsdServiceInfo) {
                        val host = s.host as? Inet4Address ?: return
                        if (host.isLoopbackAddress || s.port <= 0) return
                        val ip = host.hostAddress ?: return
                        val role = s.attributes["role"]?.let { String(it, Charsets.UTF_8) } ?: ""
                        val peer = LanPeer(
                            ip = ip,
                            port = s.port,
                            name = s.serviceName ?: "LanVid PC",
                            os = "lan",
                            simd = "mDNS",
                            rttMs = 1,
                            role = role
                        )
                        synchronized(peersLock) {
                            mdnsPeers["${peer.ip}:${peer.port}"] = peer
                        }
                        publishMerged()
                    }
                })
            }
        }
    }

    private fun stopMdns() {
        val l = listener ?: return
        listener = null
        runCatching { nsd?.stopServiceDiscovery(l) }
    }

    private fun acquireMulticastLock() {
        if (lock != null) return
        runCatching {
            lock = wifi?.createMulticastLock("LanVidCameraDiscovery")?.apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    private fun releaseMulticastLock() {
        runCatching {
            if (lock?.isHeld == true) lock?.release()
        }
        lock = null
    }

    private fun roleRank(role: String): Int = when (role.lowercase()) {
        "video" -> 0
        "" -> 1
        else -> 2
    }

    private fun publishMerged() {
        val merged = synchronized(peersLock) {
            val map = LinkedHashMap<String, LanPeer>()
            // UDP replies carry full OS/SIMD/RTT + exact stream port; mDNS fills in if broadcast is blocked
            for ((k, v) in mdnsPeers) map[k] = v
            for ((k, v) in udpPeers) map[k] = v
            map.values.sortedWith(compareBy<LanPeer>({ roleRank(it.role) }, { it.rttMs }, { it.name }))
        }
        main.post {
            peers.clear()
            peers.addAll(merged)
        }
    }
}
