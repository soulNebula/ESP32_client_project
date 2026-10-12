// Kotlin
// 设置页：扫描连板子、下发 WiFi、看 base
package com.smarthome.ble

import android.os.Bundle
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.smarthome.ble.ble.BlePermissions
import com.smarthome.ble.ble.BlePhase
import com.smarthome.ble.ble.DiscoveredDevice
import com.smarthome.ble.ui.Ui

class SettingsActivity : AppCompatActivity() {

    private val app: SmartHomeApp get() = SmartHomeApp.instance

    private lateinit var statusLine: TextView
    private lateinit var baseText: TextView
    private lateinit var scanList: LinearLayout
    private lateinit var scanHint: TextView
    private lateinit var scanButton: TextView
    private lateinit var fSsid: EditText
    private lateinit var fPass: EditText

    // 收下授权结果
    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        refreshPerm()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildRoot())

        app.onSnapshotChanged = { snap -> if (!isFinishing && !isDestroyed) render(snap) }
        app.onScanChanged = { list -> if (!isFinishing && !isDestroyed) showDevices(list) }

        refreshPerm()
        render(app.snapshot())
        showDevices(app.snapshot().scanResults)
    }

    override fun onPause() {
        super.onPause()
        // 不摘会泄漏
        app.onSnapshotChanged = null
        app.onScanChanged = null
    }

    private fun buildRoot(): View {
        val ctx = this
        val col = Ui.vertical(ctx).apply {
            setPadding(Ui.dp(ctx, 14f), Ui.dp(ctx, 14f), Ui.dp(ctx, 14f), Ui.dp(ctx, 24f))
            setBackgroundColor(Ui.BG)
        }

        col.addView(Ui.text(ctx, "扫描与设置", 20f, Ui.TEXT, bold = true))
        col.addView(
            Ui.text(
                ctx,
                "App 直接连板子的蓝牙，没 broker、没端口、不用串口。客户只填 WiFi。",
                12f, Ui.DIM,
            ).apply { topGap(ctx, 6f) },
        )

        // 1. 连接状态
        val stCard = Ui.card(ctx)
        stCard.addView(Ui.text(ctx, "连接状态", 15f, Ui.TEXT, bold = true))
        statusLine = Ui.kvLine(ctx)
        stCard.addView(statusLine)
        stCard.addView(
            Ui.rowOfButtons(
                ctx,
                Ui.button(ctx, "断开", false) { app.disconnect() },
                Ui.button(ctx, "回主界面", false) { finish() },
            ).apply { topGap(ctx, 10f) },
        )
        col.addView(stCard)

        // 2. 扫描 / 连接
        val scanCard = Ui.card(ctx)
        scanCard.addView(Ui.text(ctx, "扫描板子", 15f, Ui.TEXT, bold = true))
        scanCard.addView(
            Ui.text(ctx, "点一个就连，扫 10 秒自己停", 12f, Ui.DIM).apply { topGap(ctx, 2f) },
        )
        scanButton = Ui.button(ctx, "开始扫描", true) { toggleScan() }
        scanCard.addView(scanButton.apply { topGap(ctx, 10f) })
        scanHint = Ui.text(ctx, "", 12f, Ui.DIM)
        scanCard.addView(scanHint.apply { topGap(ctx, 8f) })
        scanList = Ui.vertical(ctx)
        scanCard.addView(scanList)
        col.addView(scanCard)

        // 3. WiFi 下发
        val wifiCard = Ui.card(ctx)
        wifiCard.addView(Ui.text(ctx, "WiFi 设置（发给板子）", 15f, Ui.TEXT, bold = true))
        fSsid = Ui.field(
            ctx, wifiCard, "WiFi 名字（SSID）",
            SmartHomeApp.Prefs.ssid(ctx), "2.4G 的，别填 5G",
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS,
        )
        fPass = Ui.field(
            ctx, wifiCard, "WiFi 密码",
            SmartHomeApp.Prefs.pass(ctx), "留空就是开放网络",
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS,
        )
        wifiCard.addView(
            Ui.button(ctx, "保存并发给设备", true) { saveWifi() }.apply { topGap(ctx, 14f) },
        )
        wifiCard.addView(
            Ui.text(ctx, "走 0x04 帧；SSID 传空表示清除，回固件里编译的默认值。", 11f, Ui.DIM)
                .apply { topGap(ctx, 8f) },
        )
        col.addView(wifiCard)

        // 4. 网页面板地址
        val baseCard = Ui.card(ctx)
        baseCard.addView(Ui.text(ctx, "网页面板地址", 15f, Ui.TEXT, bold = true))
        baseText = Ui.text(ctx, "等板子回 0x16 INFO…", 14f, Ui.DIM, bold = true)
        baseCard.addView(baseText.apply { topGap(ctx, 8f) })
        baseCard.addView(
            Ui.text(
                ctx,
                "这串 base 填进网页面板的输入框就行，不用再翻串口日志。",
                11f, Ui.DIM,
            ).apply { topGap(ctx, 6f) },
        )
        col.addView(baseCard)

        col.addView(
            Ui.text(
                ctx,
                "说明：\n" +
                    "· 不用填 broker / 端口 / TLS，蓝牙直连没有网这一层。\n" +
                    "· 板子要上电且在广播，手机蓝牙要开着，权限要给。\n" +
                    "· base 里那 6 位是板子唯一 ID，每块都不一样。",
                11f, Ui.DIM,
            ).apply { topGap(ctx, 14f) },
        )

        return ScrollView(ctx).apply {
            setBackgroundColor(Ui.BG)
            addView(col)
        }
    }

    // 加点上边距
    private fun View.topGap(ctx: android.content.Context, dp: Float) {
        val lp = layoutParams
        if (lp is LinearLayout.LayoutParams) {
            lp.topMargin = Ui.dp(ctx, dp)
            layoutParams = lp
        }
    }

    private fun refreshPerm() {
        if (!BlePermissions.granted(this)) {
            scanHint.text = "缺权限：" + BlePermissions.why()
            try {
                permLauncher.launch(BlePermissions.required())
            } catch (_: Throwable) {
            }
        }
    }

    private fun toggleScan() {
        val scanning = app.snapshot().conn.phase == BlePhase.SCANNING
        if (scanning) app.stopScan() else app.startScan()
    }

    // 列扫到的板子
    private fun showDevices(list: List<DiscoveredDevice>) {
        val ctx = this
        scanList.removeAllViews()
        val scanning = app.snapshot().conn.phase == BlePhase.SCANNING
        scanButton.text = if (scanning) "停止扫描" else "开始扫描"
        if (list.isEmpty()) {
            scanHint.text = if (scanning) "找着呢…确认板子已上电、在广播" else "还没扫到，点上面开始扫"
            return
        }
        scanHint.text = "扫到 ${list.size} 台，点一下就连："
        for (d in list) {
            val row = Ui.horizontal(ctx).apply {
                background = Ui.bgStroke(Ui.CARD2, Ui.LINE, 10f, ctx)
                val p = Ui.dp(ctx, 10f)
                setPadding(p, p, p, p)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { topMargin = Ui.dp(ctx, 6f) }
            }
            val info = Ui.vertical(ctx)
            info.addView(Ui.text(ctx, d.name, 15f, Ui.TEXT, bold = true))
            info.addView(Ui.text(ctx, "${d.address}   ${d.rssi} dBm", 11f, Ui.DIM, mono = true))
            info.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            row.addView(info)
            row.addView(
                Ui.button(ctx, "连接", true) {
                    app.stopScan()
                    app.connect(d.device, d.name)
                }.apply {
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                    )
                },
            )
            scanList.addView(row)
        }
    }

    private fun render(snap: SmartHomeApp.Snapshot) {
        val c = snap.conn
        val phase = when (c.phase) {
            BlePhase.CONNECTED -> "已连接"
            BlePhase.CONNECTING -> "连接 / 重连中"
            BlePhase.SCANNING -> "扫描中"
            BlePhase.ERROR -> "出错"
            BlePhase.IDLE -> "未连接"
        }
        statusLine.text = buildString {
            append("状态：").append(phase).append("    ").append(c.statusText)
            append("\n设备：").append(c.deviceName ?: "--").append("    ").append(c.deviceAddress ?: "--")
            append("\nMTU：").append(c.mtu?.let { "$it（单包 ${it - 3}B）" } ?: "没谈成")
            append("    Notify：").append(if (c.notifyOn) "已开" else "没开")
            append("    RSSI：").append(c.rssi?.let { "$it dBm" } ?: "--")
            if (c.reconnectCount > 0) append("\n重连次数：").append(c.reconnectCount)
            c.warning?.let { append("\n注意：").append(it) }
            c.lastError?.let { append("\n出错：").append(it) }
        }
        val b = snap.base.ifEmpty { snap.info?.base ?: "" }
        baseText.text = if (b.isEmpty()) "等板子回 0x16 INFO…" else "base = $b"
        baseText.setTextColor(if (b.isEmpty()) Ui.DIM else Ui.c(0x7FE3B0))
        scanButton.text = if (c.phase == BlePhase.SCANNING) "停止扫描" else "开始扫描"
    }

    private fun saveWifi() {
        val ssid = fSsid.text.toString().trim()
        val pass = fPass.text.toString()
        if (ssid.isEmpty() && pass.isNotEmpty()) {
            toast("要清除 WiFi 的话，密码也请留空")
            return
        }
        if (ssid.length > 32) {
            toast("SSID 太长（最多 32 字节）")
            return
        }
        if (pass.length > 64) {
            toast("密码太长（最多 64 字节）")
            return
        }
        if (app.snapshot().conn.phase != BlePhase.CONNECTED) {
            toast("还没连上板子，先在上面扫一台")
            return
        }
        SmartHomeApp.Prefs.saveWifi(this, ssid, pass)
        app.sendWifi(ssid, pass)
        toast(if (ssid.isEmpty()) "已发送清除 WiFi" else "已发给设备，板子会立刻重连")
    }

    private fun toast(msg: String) {
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show()
    }
}
