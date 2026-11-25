package com.lanvid.monitor

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/**
 * App-wide monitor engine (UI thread). All mixing and DSP happen on the PC: every change here is sent
 * immediately as kMonitorControl; the phone only plays the finished stereo bus (native AAudio).
 */
object MonitorEngine {
    private var handle = 0L
    private lateinit var prefs: SharedPreferences
    private lateinit var appContext: Context
    private val main = Handler(Looper.getMainLooper())

    var connected by mutableStateOf<Studio?>(null)
        private set
    var isDemo by mutableStateOf(false)
        private set
    val channels = mutableStateListOf<ChannelState>()
    var telemetry by mutableStateOf(Telemetry())
        private set
    var lastError by mutableStateOf<String?>(null)
    var quality by mutableStateOf(StreamQuality.K256)
        private set
    var buffer by mutableStateOf(MonitorBuffer.MS10)
        private set
    var wifiRedundancy by mutableStateOf(true)
        private set
    var dsp by mutableStateOf(DspSettings())
        private set
    var clientName by mutableStateOf(Build.MODEL ?: "Android")
        private set
    val networkInterfaces = mutableStateListOf<NetInterfaceOption>()
    var primaryNicIp by mutableStateOf("")
        private set
    var secondaryNicIp by mutableStateOf("")
        private set

    private var wifiRedundancyUserSet = false
    private val names = ByteArray(64 * 16)
    private val peaks = ByteArray(64)
    private val tel = DoubleArray(10)
    private val danteTel = DoubleArray(8)
    private var lastPackets = 0L
    private var connectStartedMs = 0L
    private var handshakeConfirmed = false

    fun init(context: Context) {
        if (handle != 0L) return
        appContext = context.applicationContext
        prefs = appContext.getSharedPreferences("lanvid_monitor", Context.MODE_PRIVATE)
        handle = MonitorNative.nativeCreate()
        quality = StreamQuality.entries.firstOrNull { it.name == prefs.getString("quality", null) } ?: StreamQuality.K256
        buffer = MonitorBuffer.entries.firstOrNull { it.name == prefs.getString("buffer", null) } ?: MonitorBuffer.MS10
        wifiRedundancyUserSet = prefs.getBoolean("wifiRedundancyUserSet", false)
        wifiRedundancy = if (wifiRedundancyUserSet) {
            prefs.getBoolean("wifiRedundancy", true)
        } else {
            quality == StreamQuality.K192 || quality == StreamQuality.K256
        }
        MonitorNative.setWifiRedundancy(handle, wifiRedundancy)
        primaryNicIp = prefs.getString("primaryNicIp", "") ?: ""
        secondaryNicIp = prefs.getString("secondaryNicIp", "") ?: ""
        refreshNetworkInterfaces()
        MonitorNative.setNetworkInterfaces(handle, primaryNicIp, secondaryNicIp)
        prefs.getString("clientName", null)?.takeIf { it.isNotBlank() }?.let { clientName = it }
        dsp = loadDsp()
    }

    val lastHost: String get() = prefs.getString("lastHost", "") ?: ""
    val lastPort: Int get() = prefs.getInt("lastPort", 5020)

    // ---- Connection ----

    fun connect(studio: Studio) {
        disconnect()
        MonitorNative.setWifiRedundancy(handle, wifiRedundancy)
        MonitorNative.setNetworkInterfaces(handle, primaryNicIp, secondaryNicIp)
        val rc = MonitorNative.nativeConnect(handle, studio.host, studio.port, clientName.take(15), quality.wireKbps, buffer.ms)
        if (rc != 0 && rc != -2) {
            lastError = "Немає з'єднання з ${studio.host}:${studio.port} (код $rc)"
            return
        }
        lastError = if (rc == -2) "Підключено, але аудіовихід AAudio не відкрився" else null
        connectStartedMs = System.currentTimeMillis()
        handshakeConfirmed = false
        connected = studio
        prefs.edit().putString("lastHost", studio.host).putInt("lastPort", studio.port).apply()
        pushDsp()
        channels.forEach { pushMix(it) }
        pushSubscribedMask()
        startService()
        main.removeCallbacks(tick)
        main.post(tick)
    }

    fun connectDemo() {
        disconnect()
        isDemo = true
        lastError = null
        connected = Studio("Demo Studio (Local Preview)", "127.0.0.1", 5020)
        channels.clear()
        val demoNames = listOf("Kick", "Snare", "Bass", "Guitar L", "Guitar R", "Keys", "Lead Vox", "Click")
        val demoGains = listOf(0f, -1.5f, -2.0f, -4.0f, -4.0f, -3.5f, 1.5f, -6.0f)
        val demoPans = listOf(0f, 0f, 0f, -0.7f, 0.7f, 0.15f, 0f, 0f)
        demoNames.forEachIndexed { i, nm ->
            val c = ChannelState(i, nm)
            c.gainDb = demoGains[i]
            c.pan = demoPans[i]
            c.peakDbfs = -18f
            channels.add(c)
        }
        demoStep = 0
        main.removeCallbacks(tick)
        main.post(tick)
    }

    fun disconnect() {
        main.removeCallbacks(tick)
        if (connected != null && !isDemo) MonitorNative.nativeDisconnect(handle)
        isDemo = false
        connected = null
        telemetry = Telemetry()
        stopService()
    }

    // ---- Stream / mixer / DSP (sent instantly) ----

    fun changeQuality(q: StreamQuality) {
        quality = q
        prefs.edit().putString("quality", q.name).apply()
        if (!wifiRedundancyUserSet) {
            wifiRedundancy = (q == StreamQuality.K192 || q == StreamQuality.K256)
            prefs.edit().putBoolean("wifiRedundancy", wifiRedundancy).apply()
            MonitorNative.setWifiRedundancy(handle, wifiRedundancy)
        }
        if (connected != null && !isDemo) MonitorNative.nativeSetStream(handle, q.wireKbps, buffer.ms)
    }

    fun changeBuffer(b: MonitorBuffer) {
        buffer = b
        prefs.edit().putString("buffer", b.name).apply()
        if (connected != null && !isDemo) MonitorNative.nativeSetStream(handle, quality.wireKbps, b.ms)
    }

    fun changeWifiRedundancy(enabled: Boolean) {
        wifiRedundancyUserSet = true
        wifiRedundancy = enabled
        prefs.edit().putBoolean("wifiRedundancy", enabled).putBoolean("wifiRedundancyUserSet", true).apply()
        MonitorNative.setWifiRedundancy(handle, enabled)
    }

    fun changeClientName(n: String) {
        clientName = n.take(15)
        prefs.edit().putString("clientName", clientName).apply()
    }

    fun refreshNetworkInterfaces() {
        val raw = runCatching { MonitorNative.listInterfaces() }.getOrDefault(emptyArray())
        val parsed = raw.mapNotNull { row ->
            val p = row.split('|')
            if (p.size < 7) return@mapNotNull null
            NetInterfaceOption(
                id = p[0],
                systemName = p[1],
                friendlyName = p[2],
                typeTag = p[3],
                ipv4 = p[4],
                speedMbps = p[5].toIntOrNull() ?: 0,
                isUp = p[6] == "1",
            )
        }
        networkInterfaces.clear()
        networkInterfaces.addAll(parsed)
    }

    fun changePrimaryNic(ip: String) {
        primaryNicIp = ip
        if (secondaryNicIp.isNotEmpty() && secondaryNicIp == primaryNicIp) {
            secondaryNicIp = ""
        }
        prefs.edit().putString("primaryNicIp", primaryNicIp).putString("secondaryNicIp", secondaryNicIp).apply()
        if (handle != 0L) MonitorNative.setNetworkInterfaces(handle, primaryNicIp, secondaryNicIp)
    }

    fun changeSecondaryNic(ip: String) {
        secondaryNicIp = if (ip.isNotEmpty() && ip == primaryNicIp) "" else ip
        prefs.edit().putString("secondaryNicIp", secondaryNicIp).apply()
        if (handle != 0L) MonitorNative.setNetworkInterfaces(handle, primaryNicIp, secondaryNicIp)
    }

    fun updateDsp(d: DspSettings) {
        if (d == dsp) return
        dsp = d
        saveDsp(d)
        pushDsp()
    }

    fun setGain(ch: ChannelState, db: Float) { ch.gainDb = db; pushMix(ch); saveMix() }
    fun setPan(ch: ChannelState, pan: Float) { ch.pan = pan; pushMix(ch); saveMix() }
    fun toggleMute(ch: ChannelState) { ch.mute = !ch.mute; pushMix(ch); pushSubscribedMask(); saveMix() }
    fun toggleSolo(ch: ChannelState) { ch.solo = !ch.solo; pushMix(ch); pushSubscribedMask(); saveMix() }

    fun resetMix() {
        channels.forEach { it.gainDb = 0f; it.pan = 0f; it.mute = false; it.solo = false; pushMix(it) }
        pushSubscribedMask()
        saveMix()
    }

    private fun computeSubscribedMask(): Long {
        if (channels.isEmpty()) return -1L
        val anySolo = channels.any { it.solo }
        var mask = 0L
        for (c in channels) {
            if (c.index in 0..63) {
                val active = if (anySolo) (c.solo && !c.mute) else !c.mute
                if (active) mask = mask or (1L shl c.index)
            }
        }
        return mask
    }

    private fun pushSubscribedMask() {
        if (connected != null && !isDemo) {
            MonitorNative.setSubscribedMask(handle, computeSubscribedMask())
        }
    }

    private fun pushMix(c: ChannelState) {
        if (connected != null && !isDemo) MonitorNative.nativeSetChannelMix(handle, c.index, c.gainDb, c.pan, c.mute, c.solo)
    }

    private fun pushDsp() {
        if (connected != null && !isDemo) MonitorNative.nativeSetDsp(handle, dsp.params(), dsp.flags())
    }

    private var demoStep = 0

    // ---- Channel names / VU / telemetry from the PC (15 Hz) ----

    private val tick = object : Runnable {
        override fun run() {
            if (isDemo) updateDemoTick() else pullMeta()
            main.postDelayed(this, 66)
        }
    }

    private fun updateDemoTick() {
        if (connected == null) return
        demoStep++
        val anySolo = channels.any { it.solo }
        var maxBusDb = -60f
        for (c in channels) {
            val audible = if (anySolo) (c.solo && !c.mute) else !c.mute
            val phase = demoStep * 0.22 + c.index * 0.85
            val beat = kotlin.math.abs(kotlin.math.sin(phase)).toFloat()
            val rawDb = -26f + beat * 20f
            c.peakDbfs = if (audible) (rawDb + c.gainDb).coerceIn(-60f, -1f) else -60f
            if (audible && c.peakDbfs > maxBusDb) maxBusDb = c.peakDbfs
        }
        val mixPeak = if (dsp.masterMute) -60f else (maxBusDb + dsp.masterVolumeDb).coerceIn(-60f, -0.5f)
        val gr = if (dsp.compOn && mixPeak > dsp.compThresholdDb) {
            ((mixPeak - dsp.compThresholdDb) * (1f / dsp.compRatio - 1f)).coerceIn(-18f, 0f)
        } else 0f
        val activeCount = channels.count { if (anySolo) (it.solo && !it.mute) else !it.mute }
        telemetry = Telemetry(
            gainReductionDb = gr,
            mixPeakDbfs = mixPeak,
            bufferMs = buffer.ms.toDouble(),
            audioPackets = demoStep.toLong() * 14L,
            receiving = true,
            burstFrames = 96,
            deviceBufferFrames = 192,
            deviceRate = 48000,
            exclusive = true,
            xruns = 0,
            clockLocked = true,
            clockOffsetJitterUs = 160L,
            duplicatesRecovered = if (wifiRedundancy) (demoStep / 6).toLong() else 0L,
            activeFlowChannels = activeCount,
            effectivePlayoutMs = buffer.ms.toDouble(),
            wifiRedundancyEnabled = wifiRedundancy,
        )
    }

    private fun pullMeta() {
        if (connected == null) return
        val prevCount = channels.size
        val n = MonitorNative.nativeGetMeta(handle, names, peaks, tel).coerceIn(0, 64)
        MonitorNative.getDanteTelemetry(handle, danteTel)
        val saved = if (n > channels.size) loadMix() else emptyMap()
        for (i in 0 until n) {
            val raw = names.copyOfRange(i * 16, i * 16 + 16)
            val len = raw.indexOf(0.toByte()).let { if (it < 0) 16 else it }
            val nm = String(raw, 0, len, Charsets.UTF_8).ifBlank { "Ch ${i + 1}" }
            if (i < channels.size) {
                val c = channels[i]
                if (c.name != nm) c.name = nm
                c.peakDbfs = peaks[i].toFloat()
            } else {  // a channel appeared on the PC: restore this phone's last mix for it
                val c = ChannelState(i, nm)
                saved[i]?.let { s -> c.gainDb = s[0]; c.pan = s[1]; c.mute = s[2] != 0f; c.solo = s[3] != 0f }
                c.peakDbfs = peaks[i].toFloat()
                channels.add(c)
                pushMix(c)
            }
        }
        while (channels.size > n) channels.removeAt(channels.size - 1)
        if (channels.size != prevCount && channels.isNotEmpty()) pushSubscribedMask()
        val packets = tel[4].toLong()
        if (n > 0 || packets > 0L) {
            handshakeConfirmed = true
            if (lastError?.startsWith("Немає відповіді") == true) lastError = null
        } else if (!handshakeConfirmed && System.currentTimeMillis() - connectStartedMs > 3500L) {
            val s = connected
            if (s != null) {
                lastError = "Немає відповіді від ${s.host}:${s.port} — перевірте Wi-Fi, LanVid Studio на ПК та Firewall (UDP 5020)"
            }
        }
        val playoutMs = if (danteTel[4] > 0.0) danteTel[4] else tel[3]
        telemetry = Telemetry(
            gainReductionDb = tel[1].toFloat(), mixPeakDbfs = tel[2].toFloat(), bufferMs = tel[3],
            audioPackets = packets, receiving = packets > lastPackets,
            burstFrames = tel[5].toInt(), deviceBufferFrames = tel[6].toInt(), deviceRate = tel[7].toInt(),
            exclusive = tel[8] != 0.0, xruns = tel[9].toInt(),
            clockLocked = danteTel[0] != 0.0,
            clockOffsetJitterUs = danteTel[1].toLong(),
            duplicatesRecovered = danteTel[2].toLong(),
            activeFlowChannels = danteTel[3].toInt(),
            effectivePlayoutMs = playoutMs,
            wifiRedundancyEnabled = danteTel[5] != 0.0,
        )
        lastPackets = packets
    }

    // ---- Foreground service keeps monitoring alive with the screen off ----

    private fun startService() {
        val i = Intent(appContext, MonitorService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) appContext.startForegroundService(i) else appContext.startService(i)
    }

    private fun stopService() {
        if (::appContext.isInitialized) appContext.stopService(Intent(appContext, MonitorService::class.java))
    }

    // ---- Persistence ----

    private fun saveDsp(d: DspSettings) {
        val j = JSONObject()
            .put("hpf", d.hpf.name).put("eqOn", d.eqOn).put("eqLow", d.eqLowDb.toDouble())
            .put("eqMid", d.eqMidDb.toDouble()).put("eqMidHz", d.eqMidHz.toDouble()).put("eqHigh", d.eqHighDb.toDouble())
            .put("compOn", d.compOn).put("thr", d.compThresholdDb.toDouble()).put("ratio", d.compRatio.toDouble())
            .put("makeup", d.compMakeupDb.toDouble()).put("vol", d.masterVolumeDb.toDouble()).put("mute", d.masterMute)
        prefs.edit().putString("dsp", j.toString()).apply()
    }

    private fun loadDsp(): DspSettings {
        val s = prefs.getString("dsp", null) ?: return DspSettings()
        return runCatching {
            val j = JSONObject(s)
            DspSettings(
                hpf = HpfMode.entries.firstOrNull { it.name == j.optString("hpf") } ?: HpfMode.OFF,
                eqOn = j.optBoolean("eqOn", true), eqLowDb = j.optDouble("eqLow", 0.0).toFloat(),
                eqMidDb = j.optDouble("eqMid", 0.0).toFloat(), eqMidHz = j.optDouble("eqMidHz", 1200.0).toFloat(),
                eqHighDb = j.optDouble("eqHigh", 0.0).toFloat(), compOn = j.optBoolean("compOn", false),
                compThresholdDb = j.optDouble("thr", -18.0).toFloat(), compRatio = j.optDouble("ratio", 3.0).toFloat(),
                compMakeupDb = j.optDouble("makeup", 0.0).toFloat(), masterVolumeDb = j.optDouble("vol", 0.0).toFloat(),
                masterMute = j.optBoolean("mute", false),
            )
        }.getOrDefault(DspSettings())
    }

    private fun saveMix() {
        val j = JSONObject()
        channels.forEach { c ->
            j.put(c.index.toString(), "${c.gainDb},${c.pan},${if (c.mute) 1 else 0},${if (c.solo) 1 else 0}")
        }
        prefs.edit().putString("mix", j.toString()).apply()
    }

    private fun loadMix(): Map<Int, List<Float>> {
        val s = prefs.getString("mix", null) ?: return emptyMap()
        return runCatching {
            val j = JSONObject(s)
            j.keys().asSequence().associate { k ->
                k.toInt() to j.getString(k).split(",").map { it.toFloat() }
            }.filterValues { it.size == 4 }
        }.getOrDefault(emptyMap())
    }
}
