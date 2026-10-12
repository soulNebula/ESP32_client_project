// Kotlin
// 蓝牙扫描、连接、订阅和分片收发
package com.smarthome.ble.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import com.smarthome.ble.data.Proto
import java.nio.charset.StandardCharsets
import java.util.ArrayDeque

enum class BlePhase { IDLE, SCANNING, CONNECTING, CONNECTED, ERROR }

data class DiscoveredDevice(
    val name: String,
    val address: String,
    val rssi: Int,
    val device: BluetoothDevice,
)

data class BleConnectionState(
    val adapterAvailable: Boolean = false,
    val bluetoothEnabled: Boolean = false,
    val phase: BlePhase = BlePhase.IDLE,
    val deviceName: String? = null,
    val deviceAddress: String? = null,
    val rssi: Int? = null,
    // 谈好的单包大小
    val mtu: Int? = null,
    val mtuRequested: Int = MTU_TARGET,
    val mtuOk: Boolean = false,
    val notifyOn: Boolean = false,
    val warning: String? = null,
    val lastError: String? = null,
    val reconnectCount: Int = 0,
    val nextRetryMs: Long = 0L,
    val statusText: String = "未连接",
) {
    companion object {
        // 协议让优先协商 517
        const val MTU_TARGET = 517
        // 谈不到就退这个
        const val MTU_FALLBACK = 247
        // 小于它就警告
        const val MTU_MIN = 256
    }
}

// 查有没有蓝牙权限
object BlePermissions {

    // 按系统版本要权限
    fun required(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    fun granted(ctx: Context): Boolean = required().all {
        ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED
    }

    fun missing(ctx: Context): List<String> = required().filter {
        ContextCompat.checkSelfPermission(ctx, it) != PackageManager.PERMISSION_GRANTED
    }

    // 被拒时给句人话
    fun why(): String =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            "Android 12 及以上要「附近的设备」权限，才能扫到并连上板子"
        } else {
            "Android 11 及以下扫蓝牙要「位置信息」权限（不拿去定位）"
        }
}

class BleManager(private val appContext: Context) {

    interface Listener {
        fun onScanResults(devices: List<DiscoveredDevice>)
        fun onConnectionState(state: BleConnectionState)
        fun onFrame(frame: ByteArray)
        fun onLog(line: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val thread = HandlerThread("ble-gatt").apply { start() }
    private val h = Handler(thread.looper)

    private val btManager: BluetoothManager? =
        appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? = btManager?.adapter

    @Volatile
    var listener: Listener? = null

    // 下面这些都只在蓝牙线程动
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var connected = false
    private var stage = Stage.IDLE
    private var notifyEnabled = false
    private val writeQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false
    private var mtuFallbackTried = false
    private var mtuToken = 0
    private var scanToken = 0
    private val seen = LinkedHashMap<String, DiscoveredDevice>()
    // 记住板子报过的名字
    private val nameCache = HashMap<String, String>()

    // 按类型攒分片
    private val fragType = HashMap<Int, StringBuilder>()

    // 界面线程也要读 MTU
    @Volatile
    private var st = BleConnectionState()

    // 断线重连
    private var autoReconnect = true
    private var manualClose = false
    private var retryCount = 0
    private var retryPending = false
    private var retryToken = 0
    private var lastDevice: BluetoothDevice? = null
    private var lastDeviceName: String? = null

    private enum class Stage { IDLE, CONNECTING, MTU, SERVICES, NOTIFY, READY }

    private companion object {
        const val MTU_TIMEOUT_MS = 4000L
        const val SCAN_TIMEOUT_MS = 10_000L
        const val RETRY_MIN_MS = 1000L
        const val RETRY_MAX_MS = 30_000L
        const val RETRY_MAX_TIMES = 20
    }

    // 查一下蓝牙开着没
    fun refreshAdapterState() {
        h.post {
            val available = adapter != null
            val enabled = try {
                adapter?.isEnabled == true
            } catch (_: SecurityException) {
                false
            }
            st = st.copy(
                adapterAvailable = available,
                bluetoothEnabled = enabled,
                phase = if (available) st.phase else BlePhase.ERROR,
                statusText = when {
                    !available -> "本机没有蓝牙适配器"
                    !enabled -> "蓝牙已关闭，请打开蓝牙"
                    st.phase == BlePhase.IDLE -> "未连接"
                    else -> st.statusText
                },
            )
            emitState()
            // 刚开蓝牙就补一次重连
            if (available && enabled) scheduleRetry()
        }
    }

    // 开始找板子
    @SuppressLint("MissingPermission")
    fun startScan() {
        h.post {
            if (!checkPerms()) {
                warn("缺少蓝牙权限，扫不了")
                return@post
            }
            val ad = adapter
            if (ad == null) {
                warn("本机没有蓝牙适配器")
                return@post
            }
            if (!ad.isEnabled) {
                warn("蓝牙没开，请先在系统里打开蓝牙")
                return@post
            }
            val scanner = ad.bluetoothLeScanner
            if (scanner == null) {
                warn("拿不到 BLE 扫描器")
                return@post
            }

            val filters = listOf(
                ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(Proto.SERVICE_UUID))
                    .build(),
            )
            val settings = ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                // 来一条报一条
                .setReportDelay(0L)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .setMatchMode(ScanSettings.MATCH_MODE_AGGRESSIVE)
                .setNumOfMatches(ScanSettings.MATCH_NUM_ONE_ADVERTISEMENT)
                .build()

            stopScanInternal()
            seen.clear()
            nameCache.clear()
            publishScanResults()
            st = st.copy(phase = BlePhase.SCANNING, statusText = "扫描中…", lastError = null)
            emitState()
            log("开始扫描（按 Service UUID 过滤）")
            try {
                scanner.startScan(filters, settings, scanCallback)
            } catch (e: Throwable) {
                // 有些机型过滤不稳
                log("带过滤的扫描起不来(${e.javaClass.simpleName})，改成不过滤")
                try {
                    scanner.startScan(null, settings, scanCallback)
                } catch (e2: Throwable) {
                    st = st.copy(phase = BlePhase.IDLE, statusText = "未连接")
                    warn("扫描起不来：${e2.message}")
                    return@post
                }
            }
            // 别一直扫
            val token = ++scanToken
            h.postDelayed({
                if (token == scanToken) {
                    log("扫满 10 秒，自动停")
                    stopScanInternal()
                }
            }, SCAN_TIMEOUT_MS)
        }
    }

    // 停止找板子
    fun stopScan() {
        h.post { stopScanInternal() }
    }

    @SuppressLint("MissingPermission")
    private fun stopScanInternal() {
        scanToken++
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Throwable) {
        }
        if (st.phase == BlePhase.SCANNING) {
            st = st.copy(phase = BlePhase.IDLE, statusText = "未连接")
        }
        emitState()
    }

    // 连上一块板子
    @SuppressLint("MissingPermission")
    fun connect(dev: BluetoothDevice, name: String?) {
        h.post {
            if (!checkPerms()) {
                warn("缺少蓝牙权限，连不上")
                return@post
            }
            cancelRetry()
            closeGatt()
            stage = Stage.CONNECTING
            connected = false
            notifyEnabled = false
            writeQueue.clear()
            writeInFlight = false
            fragType.clear()
            mtuFallbackTried = false
            manualClose = false
            autoReconnect = true
            retryCount = 0
            lastDevice = dev
            lastDeviceName = name ?: safeName(dev)

            val label = lastDeviceName ?: dev.address
            st = st.copy(
                phase = BlePhase.CONNECTING,
                deviceName = label,
                deviceAddress = dev.address,
                rssi = null,
                mtu = null,
                mtuOk = false,
                notifyOn = false,
                warning = null,
                lastError = null,
                reconnectCount = 0,
                nextRetryMs = 0L,
                statusText = "正在连接 $label …",
            )
            emitState()
            log("连接 ${dev.address} ($label)")
            openGatt(dev)
        }
    }

    @SuppressLint("MissingPermission")
    private fun openGatt(dev: BluetoothDevice) {
        try {
            gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                dev.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } else {
                dev.connectGatt(appContext, false, gattCallback)
            }
        } catch (e: SecurityException) {
            stage = Stage.IDLE
            fail("连接失败：缺 BLUETOOTH_CONNECT 权限")
        } catch (e: Throwable) {
            stage = Stage.IDLE
            fail("连接出异常：${e.message}")
        }
    }

    // 断开这块板子
    fun disconnect() {
        h.post {
            manualClose = true
            autoReconnect = false
            retryCount = 0
            cancelRetry()
            closeGatt()
            st = st.copy(
                phase = BlePhase.IDLE,
                mtu = null,
                mtuOk = false,
                notifyOn = false,
                warning = null,
                lastError = null,
                deviceName = null,
                rssi = null,
                nextRetryMs = 0L,
                statusText = "未连接",
            )
            emitState()
            log("已断开连接")
        }
    }

    // 发一条指令给板子
    fun send(type: Byte, json: String) {
        val frame = buildFrame(type, json.toByteArray(StandardCharsets.UTF_8))
        h.post {
            if (!connected || gatt == null) {
                warn("没连上，没发出去：$json")
                return@post
            }
            val limit = payloadLimit()
            if (frame.size > limit) {
                warn("帧 ${frame.size}B 比单包上限 ${limit}B 还大，没发")
                return@post
            }
            writeQueue.addLast(frame)
            pump()
        }
    }

    fun release() {
        h.post {
            manualClose = true
            autoReconnect = false
            cancelRetry()
            closeGatt()
            thread.quitSafely()
        }
    }

    fun isNotifyEnabled(): Boolean = notifyEnabled

    // 单包能放多少数据
    private fun payloadLimit(): Int = ((st.mtu ?: 23) - 3).coerceAtLeast(20)

    // 超长就切分片，bit7 表示还有
    private fun buildFrame(type: Byte, data: ByteArray): ByteArray {
        val limit = payloadLimit()
        if (data.size <= limit) {
            val out = ByteArray(data.size + 1)
            out[0] = type
            System.arraycopy(data, 0, out, 1, data.size)
            return out
        }
        val pieces = (data.size + limit - 1) / limit
        val out = ByteArray(data.size + pieces)
        var src = 0
        var dst = 0
        while (src < data.size) {
            val n = minOf(limit, data.size - src)
            val more = src + n < data.size
            out[dst++] = if (more) (type.toInt() or 0x80).toByte() else type
            System.arraycopy(data, src, out, dst, n)
            dst += n
            src += n
        }
        return out
    }

    // 收分片，bit7=0 才算一帧
    private fun handleIncoming(raw: ByteArray) {
        h.post { receive(raw) }
    }

    private fun receive(data: ByteArray) {
        if (data.isEmpty()) return
        val flags = data[0].toInt() and 0xFF
        val type = flags and 0x7F
        val more = (flags and 0x80) != 0
        val text = String(data, 1, data.size - 1, StandardCharsets.UTF_8)
        val sb = fragType.getOrPut(type) { StringBuilder() }
        if (sb.length + text.length > Proto.REASSEMBLY_MAX) {
            fragType.remove(type)
            log("帧 0x%02X 攒过 ${Proto.REASSEMBLY_MAX}B，丢掉".format(type))
            return
        }
        sb.append(text)
        if (more) return
        val whole = sb.toString()
        fragType.remove(type)
        val frame = buildFrame(type.toByte(), whole.toByteArray(StandardCharsets.UTF_8))
        main.post { listener?.onFrame(frame) }
    }

    private fun emitState() {
        val snapshot = st
        main.post { listener?.onConnectionState(snapshot) }
    }

    private fun log(line: String) {
        main.post { listener?.onLog(line) }
    }

    // 记一句提醒
    private fun warn(msg: String) {
        st = st.copy(warning = msg)
        emitState()
        log("注意 $msg")
    }

    private fun fail(msg: String) {
        st = st.copy(phase = BlePhase.ERROR, lastError = msg, statusText = msg, mtu = null)
        emitState()
        log("出错 $msg")
        scheduleRetry()
    }

    private fun checkPerms(): Boolean = BlePermissions.granted(appContext)

    @SuppressLint("MissingPermission")
    private fun safeName(dev: BluetoothDevice): String? = try {
        dev.name
    } catch (_: SecurityException) {
        null
    }

    private fun publishScanResults() {
        val list = seen.values.sortedByDescending { it.rssi }
        main.post { listener?.onScanResults(list) }
    }

    // 把连接彻底关掉
    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        stage = Stage.IDLE
        connected = false
        notifyEnabled = false
        writeQueue.clear()
        writeInFlight = false
        fragType.clear()
        mtuToken++
        val g = gatt
        gatt = null
        rxChar = null
        txChar = null
        if (g != null) {
            try {
                g.disconnect()
            } catch (_: Throwable) {
            }
            try {
                g.close()
            } catch (_: Throwable) {
            }
        }
    }

    private fun cancelRetry() {
        retryPending = false
        retryToken++
    }

    // 掉线了自己往回连，退避最长 30 秒
    private fun scheduleRetry() {
        if (!autoReconnect || manualClose || retryPending) return
        if (connected) return
        // 正在连或者正在扫就先别插队
        if (st.phase == BlePhase.CONNECTING || st.phase == BlePhase.SCANNING) return
        val dev = lastDevice ?: return
        if (retryCount >= RETRY_MAX_TIMES) {
            if (st.nextRetryMs != -1L) {
                st = st.copy(nextRetryMs = -1L, statusText = "重连太多次，先停下")
                emitState()
                log("重连 $RETRY_MAX_TIMES 次都没成，先不试了")
            }
            return
        }
        val delay = minOf(RETRY_MAX_MS, RETRY_MIN_MS shl retryCount.coerceAtMost(5))
        retryCount++
        retryPending = true
        st = st.copy(
            phase = BlePhase.CONNECTING,
            deviceName = lastDeviceName ?: st.deviceName,
            reconnectCount = retryCount,
            nextRetryMs = delay,
            statusText = "${delay / 1000} 秒后重连（第 $retryCount 次）",
        )
        emitState()
        log("${delay}ms 后重连（第 $retryCount 次）")
        val token = ++retryToken
        h.postDelayed({
            if (token != retryToken || manualClose || !autoReconnect) return@postDelayed
            retryPending = false
            if (connected) return@postDelayed
            st = st.copy(nextRetryMs = 0L)
            openGatt(dev)
        }, delay)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handleScan(result)
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            results.forEach { handleScan(it) }
        }

        override fun onScanFailed(errorCode: Int) {
            h.post {
                scanToken++
                st = st.copy(phase = BlePhase.IDLE, statusText = "未连接")
                fail("扫描失败，错误码 $errorCode")
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun handleScan(result: ScanResult) {
        val dev = result.device ?: return
        val rec = result.scanRecord

        // 名字可能晚点到
        val recName = rec?.deviceName
        val cached = try {
            dev.name
        } catch (_: SecurityException) {
            null
        }
        val name = recName ?: cached ?: nameCache[dev.address]

        // 服务号最靠得住
        val hasService = rec?.serviceUuids?.any { it.uuid == Proto.SERVICE_UUID } == true
        val nameMatches = name != null && name.startsWith(Proto.NAME_PREFIX)
        if (!hasService && !nameMatches) return
        if (name != null) nameCache[dev.address] = name

        val entry = DiscoveredDevice(
            name = name ?: "SmartHome-?",
            address = dev.address,
            rssi = result.rssi,
            device = dev,
        )
        h.post {
            val prev = seen[entry.address]
            if (prev == null || prev.rssi != entry.rssi || prev.name != entry.name) {
                seen[entry.address] = entry
                publishScanResults()
            }
            if (st.deviceAddress == entry.address && st.rssi != entry.rssi) {
                st = st.copy(rssi = entry.rssi)
                emitState()
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            h.post {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        connected = true
                        stage = Stage.MTU
                        retryCount = 0
                        retryPending = false
                        st = st.copy(
                            phase = BlePhase.CONNECTING,
                            reconnectCount = 0,
                            nextRetryMs = 0L,
                            statusText = "已连上，谈 MTU …",
                        )
                        emitState()
                        log("GATT 已连上，要 MTU=${BleConnectionState.MTU_TARGET}")
                        val ok = try {
                            g.requestMtu(BleConnectionState.MTU_TARGET)
                        } catch (e: Throwable) {
                            log("requestMtu 抛异常：${e.message}")
                            false
                        }
                        if (ok) armMtuTimeout(BleConnectionState.MTU_TARGET) else fallbackMtu()
                    }

                    BluetoothProfile.STATE_DISCONNECTED -> {
                        connected = false
                        stage = Stage.IDLE
                        notifyEnabled = false
                        writeQueue.clear()
                        writeInFlight = false
                        fragType.clear()
                        rxChar = null
                        txChar = null
                        st = st.copy(
                            phase = BlePhase.IDLE,
                            mtu = null,
                            mtuOk = false,
                            notifyOn = false,
                            rssi = null,
                            nextRetryMs = 0L,
                            statusText = "连接断了" + if (status != 0) "（status=$status）" else "",
                        )
                        emitState()
                        log("GATT 断开 (status=$status)")
                        try {
                            g.close()
                        } catch (_: Throwable) {
                        }
                        if (gatt === g) gatt = null
                        // 断了就自己往回连
                        cancelRetry()
                        scheduleRetry()
                    }

                    else -> log("连接状态变了：$newState (status=$status)")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            h.post {
                if (stage != Stage.MTU) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log("onMtuChanged 失败 status=$status")
                    fallbackMtu()
                    return@post
                }
                val load = mtu - 3
                val ok = mtu >= BleConnectionState.MTU_MIN
                st = st.copy(
                    mtu = mtu,
                    mtuOk = ok,
                    warning = if (ok) null else "MTU 只有 $mtu（单包 ${load}B），长 JSON 走分片",
                )
                emitState()
                log("MTU 谈成 $mtu（单包 ${load}B）")
                stage = Stage.SERVICES
                discoverServices(g)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            h.post {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("找服务失败 status=$status")
                    return@post
                }
                val svc = g.getService(Proto.SERVICE_UUID)
                if (svc == null) {
                    fail("没找到服务，确认连的是 SmartHome 板子")
                    return@post
                }
                val rx = svc.getCharacteristic(Proto.RX_CHAR_UUID)
                val tx = svc.getCharacteristic(Proto.TX_CHAR_UUID)
                if (rx == null || tx == null) {
                    fail("缺特征：RX=${rx != null} TX=${tx != null}")
                    return@post
                }
                rxChar = rx
                txChar = tx
                log("服务找到了：RX=0x%02X TX=0x%02X".format(rx.properties, tx.properties))
                stage = Stage.NOTIFY
                enableNotify(g, tx)
            }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            h.post {
                if (descriptor.uuid != Proto.CCCD_UUID) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    notifyEnabled = false
                    warn("开通知失败（status=$status），还是试着要一次状态")
                } else {
                    notifyEnabled = true
                    log("通知开好了（CCCD 写成功）")
                }
                stage = Stage.READY
                st = st.copy(
                    phase = BlePhase.CONNECTED,
                    notifyOn = notifyEnabled,
                    statusText = buildString {
                        append("已连接，通知 ")
                        append(if (notifyEnabled) "已开" else "没确认")
                        st.mtu?.let { append("，MTU $it") }
                    },
                )
                emitState()
                // 连上先要一次状态
                log("发 get(0x03) 要一次全量")
                send(Proto.Down.GET, "{}")
                send(Proto.Down.PING, "{}")
            }
        }

        @Deprecated("旧系统只有这个回调")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            handleIncoming(characteristic.value ?: ByteArray(0))
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleIncoming(value)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            h.post {
                writeInFlight = false
                if (status != BluetoothGatt.GATT_SUCCESS) log("出错 写入失败 status=$status")
                pump()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun discoverServices(g: BluetoothGatt) {
        val ok = try {
            g.discoverServices()
        } catch (e: Throwable) {
            log("discoverServices 抛异常：${e.message}")
            false
        }
        if (!ok) fail("discoverServices() 返回 false")
    }

    // 谈不到大 MTU 就用小的，还能靠分片
    private fun fallbackMtu() {
        val g = gatt
        if (!mtuFallbackTried && g != null) {
            mtuFallbackTried = true
            log("改试 MTU=${BleConnectionState.MTU_FALLBACK}")
            val ok = try {
                g.requestMtu(BleConnectionState.MTU_FALLBACK)
            } catch (_: Throwable) {
                false
            }
            if (ok) {
                armMtuTimeout(BleConnectionState.MTU_FALLBACK)
                return
            }
        }
        st = st.copy(
            mtuOk = false,
            warning = "MTU 没谈成，默认 23 单包只有 20B，靠自动分片顶住",
            statusText = "已连接（MTU 没谈成）",
        )
        emitState()
        val gg = gatt ?: return
        stage = Stage.SERVICES
        discoverServices(gg)
    }

    // 谈崩了也不能卡住，有分片兜底
    private fun armMtuTimeout(requested: Int) {
        val token = ++mtuToken
        h.postDelayed({
            // 已经进下一步或者又谈过一次，就作废
            if (token != mtuToken || stage != Stage.MTU) return@postDelayed
            log("${MTU_TIMEOUT_MS}ms 没等到 onMtuChanged（要的是 $requested）")
            fallbackMtu()
        }, MTU_TIMEOUT_MS)
    }

    // 打开板子的推送
    @SuppressLint("MissingPermission")
    private fun enableNotify(g: BluetoothGatt, tx: BluetoothGattCharacteristic) {
        val hasNotify = (tx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
        val hasIndicate = (tx.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0
        if (!hasNotify && !hasIndicate) warn("TX 不支持 Notify，收不到板子数据")
        try {
            val ok = g.setCharacteristicNotification(tx, true)
            if (!ok) warn("setCharacteristicNotification 返回 false")
        } catch (e: Throwable) {
            warn("开本地通知失败：${e.message}")
        }
        val cccd = tx.getDescriptor(Proto.CCCD_UUID)
        if (cccd == null) {
            warn("TX 上没有 CCCD，通知可能收不到")
            stage = Stage.READY
            st = st.copy(phase = BlePhase.CONNECTED, statusText = "已连接（没有 CCCD）")
            emitState()
            send(Proto.Down.GET, "{}")
            return
        }
        cccd.value = if (hasIndicate && !hasNotify) {
            BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
        } else {
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        }
        val started = try {
            g.writeDescriptor(cccd)
        } catch (e: Throwable) {
            log("writeDescriptor 抛异常：${e.message}")
            false
        }
        if (!started) {
            warn("写 CCCD 没成功，通知没开；还是试着要一次状态")
            stage = Stage.READY
            st = st.copy(phase = BlePhase.CONNECTED, statusText = "已连接（通知没开）")
            emitState()
            send(Proto.Down.GET, "{}")
        }
    }

    // 一包一包发
    @SuppressLint("MissingPermission")
    private fun pump() {
        if (writeInFlight) return
        val g = gatt ?: return
        val rx = rxChar ?: return
        val frame = writeQueue.pollFirst() ?: return
        writeFrame(g, rx, frame)
    }

    // 真写一包出去
    @SuppressLint("MissingPermission")
    private fun writeFrame(
        g: BluetoothGatt,
        rx: BluetoothGattCharacteristic,
        frame: ByteArray,
    ) {
        // 协议要求带响应
        rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val hex = "0x%02X".format(frame[0])
        val text = String(frame, 1, frame.size - 1, StandardCharsets.UTF_8)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                val code = g.writeCharacteristic(rx, frame, rx.writeType)
                if (code == BluetoothStatusCodes.SUCCESS) {
                    writeInFlight = true
                    log("→ $hex $text")
                } else {
                    log("出错 写被拒 (code=$code)：$text")
                }
            } else {
                @Suppress("DEPRECATION")
                rx.value = frame
                @Suppress("DEPRECATION")
                val ok = g.writeCharacteristic(rx)
                if (ok) {
                    writeInFlight = true
                    log("→ $hex $text")
                } else {
                    log("出错 写失败：$text")
                }
            }
        } catch (e: SecurityException) {
            log("出错 写被拒：没权限")
        } catch (e: Throwable) {
            log("出错 写异常：${e.message}")
        }
    }
}
