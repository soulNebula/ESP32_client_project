// Kotlin
// 手机控制面板主界面
package com.smarthome.ble

import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.smarthome.ble.ble.BlePermissions
import com.smarthome.ble.ble.BlePhase
import com.smarthome.ble.data.Command
import com.smarthome.ble.data.Proto
import com.smarthome.ble.ui.Ui

// 不做乐观 UI
class MainActivity : AppCompatActivity() {

    private val app: SmartHomeApp get() = SmartHomeApp.instance

    // 收下授权结果
    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        if (!BlePermissions.granted(this)) {
            app.note("权限被拒，蓝牙用不了；请到系统设置里打开")
        }
    }

    private lateinit var statusPill: TextView
    private lateinit var baseLine: TextView
    private lateinit var tempValue: TextView
    private lateinit var humiValue: TextView
    private lateinit var weatherText: TextView
    private lateinit var lightText: TextView
    private lateinit var netLine: TextView
    private lateinit var configLine: TextView
    private lateinit var modeBadge: TextView
    private lateinit var muteBadge: TextView
    private lateinit var errorBanner: TextView
    private lateinit var permBanner: TextView
    private lateinit var logView: TextView

    private val deviceDots = LinkedHashMap<String, View>()

    // 防连点刷屏
    private var lastSendMs = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = buildRoot()
        // 不补系统栏会被压住
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(root)
        askPermissionIfNeeded()
    }

    override fun onResume() {
        super.onResume()
        app.onSnapshotChanged = { snap -> if (!isFinishing && !isDestroyed) render(snap) }
        render(app.snapshot())
        if (BlePermissions.granted(this)) {
            // 回前台补全量
            app.refresh()
        } else {
            askPermissionIfNeeded()
        }
    }

    override fun onPause() {
        super.onPause()
        // 不摘会泄漏
        app.onSnapshotChanged = null
    }

    // 没权限就弹一次
    private fun askPermissionIfNeeded() {
        if (BlePermissions.granted(this)) return
        try {
            permLauncher.launch(BlePermissions.required())
        } catch (_: Throwable) {
        }
    }

    private fun buildRoot(): View {
        val ctx = this
        val col = Ui.vertical(ctx).apply {
            setPadding(Ui.dp(ctx, 12f), Ui.dp(ctx, 10f), Ui.dp(ctx, 12f), Ui.dp(ctx, 24f))
            setBackgroundColor(Ui.BG)
        }

        col.addView(buildHeader())
        col.addView(buildSensorRow())
        col.addView(buildWeatherCard())
        col.addView(buildNetCard())
        col.addView(buildModeRow())

        permBanner = banner(ctx, "⚠ 没有蓝牙权限，扫不到板子（点一下去申请）", 0x3A3226, 0x6B5A34) {
            askPermissionIfNeeded()
        }
        col.addView(permBanner)

        errorBanner = banner(ctx, "", 0x3A2426, 0x6B3438) { app.dismissError() }
        col.addView(errorBanner)

        col.addView(Ui.sectionTitle(ctx, "设备"))
        for (spec in Proto.SWITCHABLE_DEVICES) {
            col.addView(buildDeviceCard(spec))
        }

        // all 只管灯不含门
        col.addView(Ui.sectionTitle(ctx, "全灯（不含门）"))
        col.addView(
            Ui.rowOfButtons(
                ctx,
                Ui.button(ctx, "全灯亮", true) { sendThrottled(Command.Device(Proto.DEV_ALL, "on")) },
                Ui.button(ctx, "全灯灭", false) { sendThrottled(Command.Device(Proto.DEV_ALL, "off")) },
            ).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { topMargin = Ui.dp(ctx, 8f) }
            },
        )

        col.addView(Ui.sectionTitle(ctx, "自动模式（改完发 0x02）"))
        col.addView(
            Ui.rowOfButtons(
                ctx,
                Ui.button(ctx, "改阈值", true) { showThresholdDialog() },
                Ui.button(ctx, "手动 / 自动", false) { toggleMode() },
            ).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { topMargin = Ui.dp(ctx, 8f) }
            },
        )

        col.addView(Ui.sectionTitle(ctx, "语音快捷入口（走完整语音链路）"))
        col.addView(buildVoiceRow())

        col.addView(Ui.sectionTitle(ctx, "日志（现场排障用）"))
        logView = Ui.text(ctx, "", 11f, Ui.DIM, mono = true).apply {
            background = Ui.bgStroke(Ui.c(0x0C141C), Ui.LINE, 10f, ctx)
            val p = Ui.dp(ctx, 10f)
            setPadding(p, p, p, p)
        }
        col.addView(
            ScrollView(ctx).apply {
                addView(logView)
                background = Ui.bgStroke(Ui.c(0x0C141C), Ui.LINE, 10f, ctx)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    Ui.dp(ctx, 150f),
                ).apply { topMargin = Ui.dp(ctx, 8f) }
            },
        )

        return ScrollView(ctx).apply {
            setBackgroundColor(Ui.BG)
            addView(col)
        }
    }

    // 一条可点的提示条
    private fun banner(ctx: MainActivity, text: String, fill: Int, stroke: Int, onTap: () -> Unit): TextView =
        Ui.text(ctx, text, 13f, Ui.c(0xFFD9D9), bold = true).apply {
            background = Ui.bgStroke(Ui.c(fill), Ui.c(stroke), 10f, ctx)
            val p = Ui.dp(ctx, 12f)
            setPadding(p, p, p, p)
            visibility = View.GONE
            setOnClickListener { onTap() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = Ui.dp(ctx, 12f) }
        }

    private fun buildHeader(): View {
        val ctx = this
        val row = Ui.horizontal(ctx)

        row.addView(
            Ui.text(ctx, "智能家居蓝牙", 20f, Ui.TEXT, bold = true).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            },
        )

        statusPill = Ui.text(ctx, "未连接", 13f, Ui.c(0xDDDDDD), bold = true).apply {
            background = Ui.bgPill(Ui.c(0x333333), ctx)
            val h = Ui.dp(ctx, 8f)
            val v = Ui.dp(ctx, 4f)
            setPadding(h, v, h, v)
            gravity = Gravity.CENTER
        }
        row.addView(statusPill)

        row.addView(
            Ui.button(ctx, "菜单", false) { anchor -> showMenu(anchor) }.apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { marginStart = Ui.dp(ctx, 8f) }
            },
        )

        baseLine = Ui.text(ctx, "", 11f, Ui.DIM)
        val wrapper = Ui.vertical(ctx)
        wrapper.addView(row)
        baseLine.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = Ui.dp(ctx, 6f) }
        wrapper.addView(baseLine)
        return wrapper
    }

    private fun buildSensorRow(): View {
        val ctx = this
        val row = Ui.horizontal(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = Ui.dp(ctx, 12f) }
        }

        val tempCard = Ui.card(ctx, 1f)
        tempValue = Ui.text(ctx, "--", 34f, Ui.DIM, bold = true)
        tempCard.addView(tempValue)
        tempCard.addView(Ui.text(ctx, "温度 ℃（≥32℃ 变红，阈值板子说了算）", 12f, Ui.DIM))
        row.addView(tempCard)

        val humiCard = Ui.card(ctx, 1f)
        humiValue = Ui.text(ctx, "--", 34f, Ui.DIM, bold = true)
        humiCard.addView(humiValue)
        humiCard.addView(Ui.text(ctx, "湿度 %", 12f, Ui.DIM))
        row.addView(humiCard)

        return row
    }

    private fun buildWeatherCard(): View {
        val ctx = this
        val card = Ui.card(ctx)
        val row = Ui.horizontal(ctx)
        weatherText = Ui.text(ctx, "天气: Sunny", 19f, Ui.TEXT, bold = true).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        lightText = Ui.text(ctx, "光照 --", 13f, Ui.DIM)
        row.addView(weatherText)
        row.addView(lightText)
        card.addView(row)
        return card
    }

    private fun buildNetCard(): View {
        val ctx = this
        val card = Ui.card(ctx)
        netLine = Ui.kvLine(ctx)
        configLine = Ui.kvLine(ctx)
        card.addView(netLine)
        card.addView(configLine)
        return card
    }

    // 状态以板子为准
    private fun buildModeRow(): View {
        val ctx = this
        val row = Ui.horizontal(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = Ui.dp(ctx, 12f) }
        }

        modeBadge = Ui.text(ctx, "模式 --", 15f, Ui.TEXT, bold = true).apply {
            background = Ui.bgPill(Ui.CARD2, ctx)
            val h = Ui.dp(ctx, 16f)
            val v = Ui.dp(ctx, 9f)
            setPadding(h, v, h, v)
            isClickable = true
            setOnClickListener { toggleMode() }
        }
        row.addView(modeBadge)

        muteBadge = Ui.text(ctx, "🔔 蜂鸣器", 15f, Ui.TEXT, bold = true).apply {
            background = Ui.bgPill(Ui.CARD2, ctx)
            val h = Ui.dp(ctx, 16f)
            val v = Ui.dp(ctx, 9f)
            setPadding(h, v, h, v)
            isClickable = true
            setOnClickListener { toggleBuzzer() }
        }
        muteBadge.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { marginStart = Ui.dp(ctx, 10f) }
        row.addView(muteBadge)

        return row
    }

    private fun buildDeviceCard(spec: Proto.DeviceSpec): View {
        val ctx = this
        val card = Ui.card(ctx)

        val head = Ui.horizontal(ctx)
        head.addView(
            Ui.text(ctx, spec.label, 16f, Ui.TEXT, bold = true).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            },
        )
        val dot = View(ctx).apply {
            background = Ui.bgPill(Ui.LAMP_OFF, ctx)
            layoutParams = LinearLayout.LayoutParams(Ui.dp(ctx, 12f), Ui.dp(ctx, 12f))
        }
        deviceDots[spec.dev] = dot
        head.addView(dot)
        card.addView(head)

        val buttons = Ui.rowOfButtons(
            ctx,
            Ui.button(ctx, if (spec.onAction == "open") "开门" else "开", true) {
                sendThrottled(Command.Device(spec.dev, spec.onAction))
            },
            Ui.button(ctx, if (spec.offAction == "close") "关门" else "关", false) {
                sendThrottled(Command.Device(spec.dev, spec.offAction))
            },
        ).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = Ui.dp(ctx, 10f) }
        }
        card.addView(buttons)
        return card
    }

    // 词表外的会被拒
    private fun buildVoiceRow(): View {
        val ctx = this
        val col = Ui.vertical(ctx)
        val pair = ArrayList<View>(2)

        // 奇数项会空指针
        for ((action, label) in Proto.VOICE_SHORTCUTS) {
            pair.add(Ui.button(ctx, label, false) { sendThrottled(Command.Voice(action)) })
            if (pair.size == 2) {
                col.addView(voiceRowOf(ctx, pair))
                pair.clear()
            }
        }
        if (pair.isNotEmpty()) {
            col.addView(voiceRowOf(ctx, pair))
        }
        return col
    }

    private fun voiceRowOf(ctx: android.content.Context, buttons: List<View>): LinearLayout {
        val row = Ui.horizontal(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = Ui.dp(ctx, 8f) }
        }
        for (b in buttons) {
            b.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = Ui.dp(ctx, 4f)
                marginEnd = Ui.dp(ctx, 4f)
            }
            row.addView(b)
        }
        return row
    }

    // 只发改过的那几个
    private fun showThresholdDialog() {
        val cfg = app.snapshot().config
        if (cfg == null || !cfg.valid) {
            toast("还没收到板子的 0x14，等一下再改")
            return
        }
        val ctx = this
        val col = Ui.vertical(ctx)
        col.addView(Ui.text(ctx, "改完一条发一帧 0x02；被拒的话原因在日志和顶部红条", 12f, Ui.DIM))
        val boxes = LinkedHashMap<String, EditText>()

        // 上下限要成对改
        val now = mapOf(
            Proto.KEY_LIGHT_ON_PCT to cfg.lightOnPct,
            Proto.KEY_LIGHT_OFF_PCT to cfg.lightOffPct,
            Proto.KEY_BUZZER_ON_C to cfg.buzzerOnC,
            Proto.KEY_BUZZER_OFF_C to cfg.buzzerOffC,
            Proto.KEY_TEMP_ALERT_C to cfg.tempAlertC,
            Proto.KEY_RAIN_PCT to cfg.rainPct,
        )
        for ((key, label) in Proto.NUM_KEYS) {
            val v = now[key] ?: 0.0
            boxes[key] = Ui.field(
                ctx, col, "$label（$key）",
                if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString(),
                "整数", InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED,
            )
        }

        // 这四个发 JSON 布尔，加引号反而会变 true
        col.addView(Ui.sectionTitle(ctx, "联动开关（发 true/false）"))
        val boolNow = mapOf(
            Proto.KEY_AUTO_LIGHT to cfg.autoLight,
            Proto.KEY_AUTO_BUZZER to cfg.autoBuzzer,
            Proto.KEY_DOOR_LIGHT to cfg.doorLight,
            Proto.KEY_BUZZER_ACTIVE_LOW to cfg.buzzerActiveLow,
        )
        val switches = LinkedHashMap<String, Switch>()
        for ((key, label) in Proto.BOOL_KEYS) {
            val box = Switch(ctx).apply {
                text = "$label（$key）"
                setTextColor(Ui.TEXT)
                textSize = 13f
                isChecked = boolNow[key] ?: false
                buttonTintList = android.content.res.ColorStateList.valueOf(Ui.ACCENT)
            }
            switches[key] = box
            col.addView(box)
        }

        AlertDialog.Builder(this)
            .setTitle("在线改阈值")
            .setView(ScrollView(ctx).apply { addView(col) })
            .setPositiveButton("发送") { _, _ ->
                var n = 0
                for ((key, box) in boxes) {
                    val text = box.text.toString().trim()
                    if (text.isEmpty()) continue
                    val v = text.toIntOrNull() ?: continue
                    val old = now[key] ?: 0.0
                    if (v.toDouble() == old) continue
                    app.send(Command.Threshold(key, v))
                    n++
                }
                for ((key, box) in switches) {
                    val old = boolNow[key] ?: false
                    if (box.isChecked == old) continue
                    app.send(Command.ThresholdBool(key, box.isChecked))
                    n++
                }
                toast(if (n == 0) "没改什么" else "发了 $n 条，等板子回 ack")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun render(snap: SmartHomeApp.Snapshot) {
        val c = snap.conn
        val (label, color) = when (c.phase) {
            BlePhase.CONNECTED -> "在线" to Ui.ACCENT2
            BlePhase.CONNECTING -> "连接/重连中" to Ui.WARN2
            BlePhase.SCANNING -> "扫描中" to Ui.WARN2
            BlePhase.ERROR -> "失败" to Ui.DANGER2
            BlePhase.IDLE -> "未连接" to Ui.c(0x333333)
        }
        statusPill.text = label
        statusPill.background = Ui.bgPill(color, this)
        baseLine.text = buildString {
            append("设备：").append(c.deviceName ?: "没连")
            append("    MTU：").append(c.mtu?.let { "$it（单包 ${it - 3}B）" } ?: "没谈")
            if (c.rssi != null) append("    ").append(c.rssi).append("dBm")
            append("    ").append(c.statusText)
            if (snap.base.isNotEmpty()) append("\n网页面板 base：").append(snap.base)
        }

        permBanner.visibility = if (BlePermissions.granted(this)) View.GONE else View.VISIBLE

        val s = snap.sensor
        if (s != null && s.tempValid) {
            tempValue.text = String.format(java.util.Locale.US, "%.1f", s.temp)
            // 阈值以板子为准
            tempValue.setTextColor(if (s.tempAlert) Ui.c(0xFF4D4D) else Ui.TEXT)
        } else {
            tempValue.text = "--"
            tempValue.setTextColor(Ui.DIM)
        }
        if (s != null && s.tempValid) {
            humiValue.text = String.format(java.util.Locale.US, "%.0f", s.humi)
        } else {
            humiValue.text = "--"
        }

        val w = snap.weather
        weatherText.text = "天气: " + (
            when {
                w == null -> "Sunny"
                w.text.isNotEmpty() -> w.text
                w.raining -> "Raining!"
                else -> "Sunny"
            }
            )
        weatherText.setTextColor(if (w?.raining == true) Ui.c(0x5CB3FF) else Ui.TEXT)

        lightText.text = when {
            s == null || !s.lightValid -> "光照 --"
            s.lightIsLux -> "光照 ${Math.round(s.lux)} lux"
            else -> "光照 ${String.format(java.util.Locale.US, "%.0f", s.lightPct)}%"
        }

        val st = snap.state
        netLine.text = buildString {
            append("设备：")
            append(
                when (snap.deviceOnline) {
                    true -> "在线"
                    false -> "离线"
                    null -> "未知"
                },
            )
            if (st != null) {
                append("    WiFi ").append(if (st.wifi) "✓" else "✗")
                if (st.mqtt) append("    MQTT ✓")
                if (st.rssi != 0) append("    ").append(st.rssi).append("dBm")
                if (st.ip.isNotEmpty()) append("    ").append(st.ip)
            }
        }
        configLine.text = app.configText() +
            (snap.info?.base?.let { "    · base $it" } ?: "") +
            (snap.info?.ver?.let { "    · 固件 $it" } ?: "")

        val auto = st?.auto ?: false
        modeBadge.text = if (auto) "自动模式" else "手动模式"
        modeBadge.background = Ui.bgPill(if (auto) Ui.ACCENT2 else Ui.WARN2, this)

        val bz = st?.buzzer
        when {
            bz == null -> {
                muteBadge.text = "🔔 蜂鸣器"
                muteBadge.background = Ui.bgPill(Ui.CARD2, this)
            }
            bz.muted -> {
                muteBadge.text = "🔇 已静音"
                muteBadge.background = Ui.bgPill(Ui.DANGER2, this)
            }
            bz.power -> {
                muteBadge.text = "🔔 报警中"
                muteBadge.background = Ui.bgPill(Ui.DANGER2, this)
            }
            else -> {
                muteBadge.text = "🔔 蜂鸣器"
                muteBadge.background = Ui.bgPill(Ui.CARD2, this)
            }
        }

        for ((dev, dot) in deviceDots) {
            val power = st?.device(dev)?.power ?: false
            dot.background = Ui.bgPill(if (power) Ui.LAMP_ON else Ui.LAMP_OFF, this)
        }

        // 各类错误都落这里
        if (snap.lastError.isEmpty()) {
            errorBanner.visibility = View.GONE
        } else {
            errorBanner.visibility = View.VISIBLE
            errorBanner.text = "⚠ ${snap.lastError}（点一下关闭）"
        }

        logView.text = snap.log.joinToString("\n")
    }

    private fun sendThrottled(cmd: Command) {
        val now = System.currentTimeMillis()
        if (now - lastSendMs < SEND_COOLDOWN_MS) return
        lastSendMs = now
        app.send(cmd)
    }

    private fun toggleMode() {
        val cur = app.snapshot().state?.auto ?: false
        app.send(Command.Mode(!cur))
    }

    private fun toggleBuzzer() {
        val cur = app.snapshot().state?.buzzer?.muted ?: false
        app.send(Command.Buzzer(cur))
    }

    private fun toast(msg: String) {
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show()
    }

    private fun showMenu(anchor: View) {
        val menu = PopupMenu(this, anchor)
        menu.menu.add(0, MENU_REFRESH, 0, "刷新状态（发 0x03）")
        menu.menu.add(0, MENU_SETTINGS, 1, "扫描与设置（连板子 / 填 WiFi）")
        menu.menu.add(0, MENU_THRESHOLD, 2, "在线改阈值")
        menu.menu.add(0, MENU_RECONNECT, 3, "断开并重连")
        menu.menu.add(0, MENU_CLEAR_LOG, 4, "清空日志")
        menu.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                MENU_REFRESH -> {
                    app.refresh()
                    true
                }
                MENU_SETTINGS -> {
                    startActivity(Intent(this, SettingsActivity::class.java))
                    true
                }
                MENU_THRESHOLD -> {
                    showThresholdDialog()
                    true
                }
                MENU_RECONNECT -> {
                    val c = app.snapshot().conn.deviceAddress
                    app.disconnect()
                    if (c.isNullOrEmpty()) {
                        toast("先到设置里扫一台板子")
                    } else {
                        toast("已断开，扫到就自动连")
                        app.startScan()
                    }
                    true
                }
                MENU_CLEAR_LOG -> {
                    app.clearLog()
                    true
                }
                else -> false
            }
        }
        menu.show()
    }

    companion object {
        private const val SEND_COOLDOWN_MS = 300L
        private const val MENU_REFRESH = 1
        private const val MENU_SETTINGS = 2
        private const val MENU_THRESHOLD = 3
        private const val MENU_RECONNECT = 4
        private const val MENU_CLEAR_LOG = 5
    }
}
