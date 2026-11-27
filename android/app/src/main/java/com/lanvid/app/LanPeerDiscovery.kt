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
import java.util.ArrayDeque
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Bulletproof 3-layer LAN discovery for LanVid Camera:
 *   1. Acquires Android WifiManager.MulticastLock so Wi-Fi chips never drop broadcast/mDNS replies.
 *   2. Continuous mDNS / Bonjour (_lanvid._udp) via NsdManager (serialized resolve on API < 34,
 *      tracked ServiceInfoCallback cleanup on API 34+).
 *   3. Continuous UDP Broadcast probe via LanVidNative.scanLanPeers (:5023 Video, :5021 Audio, :5099 Legacy).
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
    private val released = AtomicBoolean(false)

    private val mdnsPeers = LinkedHashMap<String, LanPeer>()
    private val udpPeers = LinkedHashMap<String, LanPeer>()
    private val peersLock = Any()

    // Track API 34+ callbacks for clean unregistration in stopMdns()
    private val activeInfoCallbacks = LinkedHashSet<Any>()
    // Serialize API < 34 resolveService() calls (NsdManager rejects concurrent resolves)
    private val resolveQueue = ArrayDeque<NsdServiceInfo>()
    private var resolveInFlight = false

    val peers = mutableStateListOf<LanPeer>()
    var scanning by mutableStateOf(false)
        private set

    fun start() {
        if (released.get()) return
        if (!running.compareAndSet(false, true)) {
            triggerUdpScan()
            return
        }
        acquireMulticastLock()
        startMdns()
        main.post(udpLoop)
    }

    fun refreshNow() {
        if (released.get()) return
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

    fun release() {
        if (!released.compareAndSet(false, true)) return
        stop()
        io.shutdownNow()
    }

    private val udpLoop = object : Runnable {
        override fun run() {
            if (!running.get()) return
            triggerUdpScan()
            main.postDelayed(this, 2500L)
        }
    }

    private fun triggerUdpScan() {
        if (!running.get() || released.get()) return
        main.post { scanning = true }
        runCatching {
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
            if (released.get()) return
            val cb = object : NsdManager.ServiceInfoCallback {
                override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) {
                    synchronized(peersLock) { activeInfoCallbacks.remove(this) }
                }
                override fun onServiceUpdated(s: NsdServiceInfo) {
                    val ip = s.hostAddresses.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }?.hostAddress
                        ?: return
                    if (s.port <= 0) return
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
                        activeInfoCallbacks.remove(this)
                    }
                    publishMerged()
                    runCatching { mgr.unregisterServiceInfoCallback(this) }
                }
                override fun onServiceLost() = Unit
                override fun onServiceInfoCallbackUnregistered() {
                    synchronized(peersLock) { activeInfoCallbacks.remove(this) }
                }
            }
            synchronized(peersLock) { activeInfoCallbacks.add(cb) }
            val ok = runCatching { mgr.registerServiceInfoCallback(info, io, cb) }.isSuccess
            if (!ok) {
                synchronized(peersLock) { activeInfoCallbacks.remove(cb) }
            }
        } else {
            synchronized(peersLock) {
                if (resolveQueue.size < 32) {
                    resolveQueue.addLast(info)
                }
                if (!resolveInFlight) {
                    resolveInFlight = true
                    pumpNextLegacyResolveLocked(mgr)
                }
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun pumpNextLegacyResolveLocked(mgr: NsdManager) {
        if (!running.get() || resolveQueue.isEmpty()) {
            resolveInFlight = false
            return
        }
        val next = resolveQueue.removeFirst()
        val ok = runCatching {
            mgr.resolveService(next, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                    synchronized(peersLock) { pumpNextLegacyResolveLocked(mgr) }
                }
                override fun onServiceResolved(s: NsdServiceInfo) {
                    val host = s.host as? Inet4Address
                    if (host != null && !host.isLoopbackAddress && s.port > 0) {
                        val ip = host.hostAddress
                        if (ip != null) {
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
                    }
                    synchronized(peersLock) { pumpNextLegacyResolveLocked(mgr) }
                }
            })
        }.isSuccess
        if (!ok) {
            pumpNextLegacyResolveLocked(mgr)
        }
    }

    private fun stopMdns() {
        val l = listener
        listener = null
        if (l != null) {
            runCatching { nsd?.stopServiceDiscovery(l) }
        }
        val callbacksCopy: List<Any>
        synchronized(peersLock) {
            resolveQueue.clear()
            resolveInFlight = false
            callbacksCopy = activeInfoCallbacks.toList()
            activeInfoCallbacks.clear()
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val mgr = nsd ?: return
            for (cb in callbacksCopy) {
                (cb as? NsdManager.ServiceInfoCallback)?.let {
                    runCatching { mgr.unregisterServiceInfoCallback(it) }
                }
            }
        }
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
