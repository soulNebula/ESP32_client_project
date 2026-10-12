// Kotlin
// 数据模型与帧解析
package com.smarthome.ble.data

import org.json.JSONObject

// 一路设备的开关
data class DeviceState(
    val power: Boolean = false,
    val level: Int = 0,
    val valid: Boolean = false,
)

// 蜂鸣器
data class BuzzerState(
    val power: Boolean = false,
    val muted: Boolean = false,
    val valid: Boolean = false,
)

// 0x11 设备状态
data class StatePayload(
    val devices: Map<String, DeviceState> = emptyMap(),
    val buzzer: BuzzerState = BuzzerState(),
    val auto: Boolean = false,
    val modeText: String = "",
    val wifi: Boolean = false,
    val mqtt: Boolean = false,
    val rssi: Int = 0,
    val ip: String = "",
) {
    fun device(dev: String): DeviceState = devices[dev] ?: DeviceState()
}

// 0x12 温湿度光照雨滴
data class SensorPayload(
    val temp: Double = 0.0,
    val humi: Double = 0.0,
    val tempValid: Boolean = false,
    val tempAlert: Boolean = false,
    val lux: Double = 0.0,
    val lightPct: Double = 0.0,
    val lightIsLux: Boolean = false,
    val lightValid: Boolean = false,
    val rain: Double = 0.0,
    val raining: Boolean = false,
    val rainValid: Boolean = false,
)

// 0x13 天气卡片
data class WeatherPayload(
    val raining: Boolean = false,
    val text: String = "",
    val rainPct: Double = 0.0,
    val valid: Boolean = false,
)

// 0x14 当前阈值和模式
data class ConfigPayload(
    val modeText: String = "",
    val auto: Boolean = false,
    val lightOnPct: Double = 0.0,
    val lightOffPct: Double = 0.0,
    val buzzerOnC: Double = 0.0,
    val buzzerOffC: Double = 0.0,
    val tempAlertC: Double = 0.0,
    val rainPct: Double = 0.0,
    val autoLight: Boolean = true,
    val autoBuzzer: Boolean = true,
    val doorLight: Boolean = true,
    val buzzerActiveLow: Boolean = false,
    val valid: Boolean = false,
)

// 0x15 指令回执
data class AckPayload(
    val dev: String = "",
    val action: String = "",
    val ok: Boolean = false,
    val detail: String = "",
)

// 0x16 设备信息
data class InfoPayload(
    val name: String = "",
    val ver: String = "",
    val base: String = "",
    val bleMtu: Int = 0,
)

object Frames {

    // 解析 0x11
    fun parseState(text: String): StatePayload? {
        val root = Json.parseObject(text) ?: return null
        val devices = LinkedHashMap<String, DeviceState>()
        val names = Proto.SWITCHABLE_DEVICES.map { it.dev } + Proto.DEV_BUZZER
        for (dev in names) {
            val o = Json.obj(root, dev) ?: continue
            devices[dev] = DeviceState(
                power = Json.bool(o, "power"),
                level = Json.int(o, "level"),
                valid = true,
            )
        }
        val bz = Json.obj(root, Proto.DEV_BUZZER)
        return StatePayload(
            devices = devices,
            buzzer = BuzzerState(
                power = Json.bool(bz, "power"),
                muted = Json.bool(bz, "muted"),
                valid = bz != null,
            ),
            auto = Json.bool(root, "auto"),
            modeText = Json.str(root, "mode"),
            wifi = Json.bool(root, "wifi"),
            mqtt = Json.bool(root, "mqtt"),
            rssi = Json.int(root, "rssi"),
            ip = Json.str(root, "ip"),
        )
    }

    // 解析 0x12
    fun parseSensor(text: String): SensorPayload? {
        val o = Json.parseObject(text) ?: return null
        return SensorPayload(
            temp = Json.dbl(o, "temp"),
            humi = Json.dbl(o, "humi"),
            tempValid = Json.bool(o, "temp_valid"),
            // 别自己再判
            tempAlert = Json.bool(o, "temp_alert"),
            lux = Json.dbl(o, "lux"),
            lightPct = Json.dbl(o, "light_pct"),
            lightIsLux = Json.bool(o, "light_is_lux"),
            lightValid = Json.bool(o, "light_valid"),
            rain = Json.dbl(o, "rain"),
            raining = Json.bool(o, "raining"),
            rainValid = Json.bool(o, "rain_valid"),
        )
    }

    // 解析 0x13
    fun parseWeather(text: String): WeatherPayload? {
        val o = Json.parseObject(text) ?: return null
        return WeatherPayload(
            raining = Json.bool(o, "raining"),
            text = Json.str(o, "text"),
            rainPct = Json.dbl(o, "rain_pct"),
            valid = true,
        )
    }

    // 解析 0x14
    fun parseConfig(text: String): ConfigPayload? {
        val o = Json.parseObject(text) ?: return null
        return ConfigPayload(
            modeText = Json.str(o, "mode"),
            auto = Json.bool(o, "auto"),
            lightOnPct = Json.dbl(o, Proto.KEY_LIGHT_ON_PCT),
            lightOffPct = Json.dbl(o, Proto.KEY_LIGHT_OFF_PCT),
            buzzerOnC = Json.dbl(o, Proto.KEY_BUZZER_ON_C),
            buzzerOffC = Json.dbl(o, Proto.KEY_BUZZER_OFF_C),
            tempAlertC = Json.dbl(o, Proto.KEY_TEMP_ALERT_C),
            rainPct = Json.dbl(o, Proto.KEY_RAIN_PCT),
            autoLight = Json.bool(o, Proto.KEY_AUTO_LIGHT, true),
            autoBuzzer = Json.bool(o, Proto.KEY_AUTO_BUZZER, true),
            doorLight = Json.bool(o, Proto.KEY_DOOR_LIGHT, true),
            buzzerActiveLow = Json.bool(o, Proto.KEY_BUZZER_ACTIVE_LOW),
            valid = true,
        )
    }

    // 解析 0x15
    fun parseAck(text: String): AckPayload? {
        val o = Json.parseObject(text) ?: return null
        return AckPayload(
            dev = Json.str(o, "dev"),
            action = Json.str(o, "action"),
            ok = Json.bool(o, "ok"),
            detail = Json.str(o, "detail"),
        )
    }

    // 解析 0x16
    fun parseInfo(text: String): InfoPayload? {
        val o = Json.parseObject(text) ?: return null
        return InfoPayload(
            name = Json.str(o, "name"),
            ver = Json.str(o, "ver"),
            base = Json.str(o, "base"),
            bleMtu = Json.int(o, "ble_mtu"),
        )
    }

    // 按首字节分给各家
    fun decode(type: Byte, text: String): Any? = when (type) {
        Proto.Up.STATE -> parseState(text)
        Proto.Up.SENSOR -> parseSensor(text)
        Proto.Up.WEATHER -> parseWeather(text)
        Proto.Up.CONFIG -> parseConfig(text)
        Proto.Up.ACK -> parseAck(text)
        Proto.Up.INFO -> parseInfo(text)
        else -> null
    }
}

// 下行指令，拼成 JSON
sealed class Command {

    data class Device(val dev: String, val action: String, val level: Int = -1) : Command()

    // 归一化成 mode
    data class Mode(val auto: Boolean) : Command()

    // off 进的是静音锁
    data class Buzzer(val on: Boolean) : Command()

    // 走完整语音链路
    data class Voice(val action: String) : Command()

    // 改一个阈值
    data class Threshold(val key: String, val value: Int) : Command()

    // 改一个开关型阈值
    data class ThresholdBool(val key: String, val value: Boolean) : Command()

    // 让板子重发全量
    object RequestAll : Command()

    fun wireJson(): String = when (this) {
        is Device -> {
            val o = JSONObject()
            o.put("dev", dev)
            o.put("action", action)
            if (level >= 0) o.put("value", level)
            o.toString()
        }
        is Mode -> Proto.cmdMode(auto)
        is Buzzer -> Proto.cmdBuzzer(on)
        is Voice -> Proto.cmdVoice(action)
        is Threshold -> Proto.cmdConfigInt(key, value)
        is ThresholdBool -> Proto.cmdConfigBool(key, value)
        RequestAll -> GET_PAYLOAD
    }

    fun humanText(): String = when (this) {
        is Device -> "$dev $action" + if (level >= 0) " ($level)" else ""
        is Mode -> if (auto) "切到自动模式" else "切到手动模式"
        is Buzzer -> if (on) "解除蜂鸣器静音" else "蜂鸣器静音"
        is Voice -> "语音指令 $action"
        is Threshold -> "改阈值 $key=$value"
        is ThresholdBool -> "改阈值 $key=$value"
        RequestAll -> "请求全量状态"
    }

    companion object {
        // 0x03 的数据板子不看
        const val GET_PAYLOAD = "{}"
    }
}
