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
import java.net.Inet4Address

/**
 * Finds LanVid Studio on the LAN through mDNS / DNS-SD ("_lanvid._udp", published by Studio while its mobile
 * monitor server runs). Resolves one service at a time (NsdManager allows a single pending resolve).
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

    fun start() {
        stop()
        lock = wifi.createMulticastLock("lanvid-monitor-mdns").apply { setReferenceCounted(false); acquire() }
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {
                main.post { status = "Пошук LanVid Studio у Wi-Fi мережі…" }
            }
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                main.post { status = "Пошук недоступний (код $errorCode) — підключіться за IP" }
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
        nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l)
    }

    fun stop() {
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        listener = null
        pending.clear()
        resolving = false
        lock?.let { if (it.isHeld) it.release() }
        lock = null
    }

    private fun enqueue(info: NsdServiceInfo) {
        if (studios.any { it.name == info.serviceName } || pending.any { it.serviceName == info.serviceName }) return
        pending.addLast(info)
        resolveNext()
    }

    @Suppress("DEPRECATION")  // resolveService: still the portable path on API 28..33
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
                    if (host != null && studios.none { it.name == serviceInfo.serviceName }) {
                        studios.add(Studio(serviceInfo.serviceName, host, serviceInfo.port))
                    }
                    resolving = false
                    resolveNext()
                }
            }
        })
    }

    companion object {
        const val SERVICE_TYPE = "_lanvid._udp"
    }
}
