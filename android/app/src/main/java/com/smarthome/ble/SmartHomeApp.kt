// Kotlin
// 全局状态，持有蓝牙管理器和设置
package com.smarthome.ble

import android.app.Application
import android.content.Context
import android.bluetooth.BluetoothDevice
import android.os.Handler
import android.os.Looper
import com.smarthome.ble.ble.BleConnectionState
import com.smarthome.ble.ble.BleManager
import com.smarthome.ble.ble.BlePhase
import com.smarthome.ble.ble.DiscoveredDevice
import com.smarthome.ble.data.AckPayload
import com.smarthome.ble.data.Command
import com.smarthome.ble.data.ConfigPayload
import com.smarthome.ble.data.Frames
import com.smarthome.ble.data.InfoPayload
import com.smarthome.ble.data.Proto
import com.smarthome.ble.data.SensorPayload
import com.smarthome.ble.data.StatePayload
import com.smarthome.ble.data.WeatherPayload

class SmartHomeApp : Application(), BleManager.Listener {

    object Prefs {
        private const val NAME = "smart_home_ble"
        private const val KEY_SSID = "wifi_ssid"
        private const val KEY_PASS = "wifi_pass"
        private const val KEY_DEV_ADDR = "last_device_addr"
        private const val KEY_DEV_NAME = "last_device_name"

        fun ssid(c: Context): String = sp(c).getString(KEY_SSID, "") ?: ""

        fun pass(c: Context): String = sp(c).getString(KEY_PASS, "") ?: ""

        fun device(c: Context): String = sp(c).getString(KEY_DEV_ADDR, "") ?: ""

        fun deviceName(c: Context): String = sp(c).getString(KEY_DEV_NAME, "") ?: ""

        // WiFi 是客户唯一要填的
        fun saveWifi(c: Context, ssid: String, pass: String) {
            sp(c).edit().putString(KEY_SSID, ssid).putString(KEY_PASS, pass).apply()
        }

        fun saveDevice(c: Context, address: String, name: String) {
            sp(c).edit().putString(KEY_DEV_ADDR, address).putString(KEY_DEV_NAME, name).apply()
        }

        private fun sp(c: Context) = c.getSharedPreferences(NAME, Context.MODE_PRIVATE)
    }

    data class Snapshot(
        val conn: BleConnectionState = BleConnectionState(),
        val scanResults: List<DiscoveredDevice> = emptyList(),
        val deviceOnline: Boolean? = null,
        val state: StatePayload? = null,
        val sensor: SensorPayload? = null,
        val weather: WeatherPayload? = null,
        val config: ConfigPayload? = null,
        val info: InfoPayload? = null,
        val lastError: String = "",
        val base: String = "",
        val log: List<String> = emptyList(),
    )

    var onSnapshotChanged: ((Snapshot) -> Unit)? = null
    var onScanChanged: ((List<DiscoveredDevice>) -> Unit)? = null

    lateinit var ble: BleManager
        private set

    private val main = Handler(Looper.getMainLooper())
    private val logLines = ArrayList<String>(LOG_MAX)

    private var conn = BleConnectionState()
    private var scanResults: List<DiscoveredDevice> = emptyList()
    private var deviceOnline: Boolean? = null
    private var statePayload: StatePayload? = null
    private var sensorPayload: SensorPayload? = null
    private var weatherPayload: WeatherPayload? = null
    private var configPayload: ConfigPayload? = null
    private var infoPayload: InfoPayload? = null
    private var lastError = ""
    private var base = ""

    override fun onCreate() {
        super.onCreate()
        instance = this
        ble = BleManager(applicationContext)
        ble.listener = this
        ble.refreshAdapterState()
        log("App 启动")
        val last = Prefs.device(this)
        if (last.isEmpty()) {
            log("还没连过板子：菜单 →「扫描与设置」里扫一台")
        } else {
            log("上次连的是 ${Prefs.deviceName(this)}，扫到就自动连")
            ble.startScan()
        }
    }

    // 扫到上次那块板子就自己连
    override fun onScanResults(devices: List<DiscoveredDevice>) {
        scanResults = devices
        onScanChanged?.invoke(devices)
        val want = Prefs.device(this)
        if (want.isEmpty() || conn.phase == BlePhase.CONNECTED) return
        val hit = devices.firstOrNull { it.address == want } ?: return
        log("扫到上次的板子，自动连 ${hit.name}")
        ble.stopScan()
        ble.connect(hit.device, hit.name)
    }

    override fun onConnectionState(state: BleConnectionState) {
        val prev = conn
        conn = state
        if (prev.phase != BlePhase.CONNECTED && state.phase == BlePhase.CONNECTED) {
            log("✓ 已连接 ${state.deviceName ?: state.deviceAddress}，MTU=${state.mtu ?: "?"}")
            Prefs.saveDevice(this, state.deviceAddress ?: "", state.deviceName ?: "")
        }
        if (prev.phase == BlePhase.CONNECTED && state.phase != BlePhase.CONNECTED) {
            log("连接不在了：${state.statusText}")
            deviceOnline = null
        }
        publishSnapshot()
    }

    override fun onFrame(frame: ByteArray) {
        if (frame.isEmpty()) return
        val type = frame[0]
        val text = String(frame, 1, frame.size - 1, Charsets.UTF_8)
        when (val parsed = Frames.decode(type, text)) {
            is StatePayload -> {
                statePayload = parsed
                deviceOnline = true
                log("← STATE $text")
            }
            is SensorPayload -> {
                sensorPayload = parsed
                log("← SENSOR $text")
            }
            is WeatherPayload -> {
                weatherPayload = parsed
                log("← WEATHER $text")
            }
            is ConfigPayload -> {
                configPayload = parsed
                log("← CONFIG $text")
            }
            is AckPayload -> {
                if (parsed.ok) {
                    lastError = ""
                    log("✓ ${parsed.dev} ${parsed.action} ${parsed.detail}")
                } else {
                    lastError = "${parsed.dev} ${parsed.action} 被拒：${parsed.detail.ifEmpty { "没说原因" }}"
                    log("✗ $lastError")
                }
            }
            is InfoPayload -> {
                infoPayload = parsed
                if (parsed.base.isNotEmpty()) base = parsed.base
                log("← INFO 设备 ${parsed.name} 版本 ${parsed.ver} base=${parsed.base}")
            }
            else -> log("← 不认识的帧 0x%02X，忽略：${text.take(60)}".format(type))
        }
        publishSnapshot()
    }

    override fun onLog(line: String) {
        log(line)
        publishSnapshot()
    }

    // 不做乐观 UI，只记一句已发出
    fun send(cmd: Command, quiet: Boolean = false): Boolean {
        if (conn.phase != BlePhase.CONNECTED) {
            log("没连上，指令没发出去：${cmd.humanText()}")
            // 自动刷新别刷红条
            if (!quiet) lastError = "没连上，指令没发出去"
            publishSnapshot()
            return false
        }
        val type = when (cmd) {
            is Command.Threshold -> Proto.Down.CONFIG
            is Command.ThresholdBool -> Proto.Down.CONFIG
            Command.RequestAll -> Proto.Down.GET
            else -> Proto.Down.CMD
        }
        ble.send(type, cmd.wireJson())
        log("→ ${cmd.humanText()}  已发送")
        publishSnapshot()
        return true
    }

    // 把 WiFi 交给板子
    fun sendWifi(ssid: String, pass: String): Boolean {
        if (conn.phase != BlePhase.CONNECTED) {
            lastError = "没连上，WiFi 没发出去"
            log(lastError)
            publishSnapshot()
            return false
        }
        ble.send(Proto.Down.WIFI, Proto.cmdWifi(ssid, pass))
        log("→ 下发 WiFi「$ssid」已发送")
        publishSnapshot()
        return true
    }

    fun startScan() = ble.startScan()

    fun stopScan() = ble.stopScan()

    fun connect(dev: BluetoothDevice, name: String?) = ble.connect(dev, name)

    fun disconnect() = ble.disconnect()

    fun refresh() {
        send(Command.RequestAll, quiet = true)
    }

    fun clearLog() {
        logLines.clear()
        log("日志已清空")
        publishSnapshot()
    }

    fun dismissError() {
        lastError = ""
        publishSnapshot()
    }

    // 界面也想记一句
    fun note(line: String) {
        log(line)
        publishSnapshot()
    }

    fun snapshot(): Snapshot = Snapshot(
        conn = conn,
        scanResults = scanResults,
        deviceOnline = deviceOnline,
        state = statePayload,
        sensor = sensorPayload,
        weather = weatherPayload,
        config = configPayload,
        info = infoPayload,
        lastError = lastError,
        base = base,
        log = ArrayList(logLines),
    )

    // 阈值面板显示的一行
    fun configText(): String {
        val c = configPayload ?: return "阈值：等板子报 0x14"
        if (!c.valid) return "阈值：等板子报 0x14"
        return "阈值：光照 ${num(c.lightOnPct)}/${num(c.lightOffPct)}% · 蜂鸣器 " +
            "${num(c.buzzerOnC)}/${num(c.buzzerOffC)}℃ · 告警 ${num(c.tempAlertC)}℃ · " +
            "雨滴 ${num(c.rainPct)}%\n联动：光控${onOff(c.autoLight)} · 蜂鸣器${onOff(c.autoBuzzer)} · " +
            "开门亮灯${onOff(c.doorLight)} · 低电平${onOff(c.buzzerActiveLow)}"
    }

    private fun onOff(v: Boolean): String = if (v) "开" else "关"

    private fun num(v: Double): String =
        if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()

    private fun log(line: String) {
        val t = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date())
        logLines.add("[$t] $line")
        while (logLines.size > LOG_MAX) logLines.removeAt(0)
    }

    private fun publishSnapshot() {
        val cb = onSnapshotChanged ?: return
        val snap = snapshot()
        main.post { cb(snap) }
    }

    companion object {
        private const val LOG_MAX = 80

        // App 实例
        lateinit var instance: SmartHomeApp
            private set
    }
}
