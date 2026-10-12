// Kotlin
// 蓝牙协议的 UUID、帧类型和键名
package com.smarthome.ble.data

import java.util.UUID

object Proto {

    val SERVICE_UUID: UUID = UUID.fromString("5a1c0001-1e2f-4a3b-9c4d-5e6f708192a3")

    // 手机写到板子
    val RX_CHAR_UUID: UUID = UUID.fromString("5a1c0002-1e2f-4a3b-9c4d-5e6f708192a3")

    // 板子推到手机
    val TX_CHAR_UUID: UUID = UUID.fromString("5a1c0003-1e2f-4a3b-9c4d-5e6f708192a3")

    // 开通知的那一格
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // 设备名前缀
    const val NAME_PREFIX = "SmartHome-"

    // 一帧最长这么多
    const val REASSEMBLY_MAX = 1024

    // 手机发下去
    object Down {
        const val CMD: Byte = 0x01
        const val CONFIG: Byte = 0x02
        const val GET: Byte = 0x03
        const val WIFI: Byte = 0x04
        const val PING: Byte = 0x05
    }

    // 板子报上来
    object Up {
        const val STATE: Byte = 0x11
        const val SENSOR: Byte = 0x12
        const val WEATHER: Byte = 0x13
        const val CONFIG: Byte = 0x14
        const val ACK: Byte = 0x15
        const val INFO: Byte = 0x16
    }

    // 设备代号，要对上固件
    const val DEV_LED_LIVING = "led_living"
    const val DEV_LED_KITCHEN = "led_kitchen"
    const val DEV_LED_BEDROOM = "led_bedroom"
    const val DEV_LED_BATH = "led_bath"
    const val DEV_DOOR = "door"
    const val DEV_ALL = "all"
    const val DEV_BUZZER = "buzzer"
    const val DEV_VOICE = "voice"

    // 界面按这个顺序摆
    val SWITCHABLE_DEVICES = listOf(
        DeviceSpec(DEV_LED_LIVING, "客厅灯", "on", "off"),
        DeviceSpec(DEV_LED_KITCHEN, "厨房灯", "on", "off"),
        DeviceSpec(DEV_LED_BEDROOM, "卧室灯", "on", "off"),
        DeviceSpec(DEV_LED_BATH, "浴室灯", "on", "off"),
        DeviceSpec(DEV_DOOR, "门", "open", "close"),
    )

    data class DeviceSpec(
        val dev: String,
        val label: String,
        val onAction: String,
        val offAction: String,
    )

    // 阈值键名，和固件一致
    const val KEY_MODE = "mode"
    const val KEY_LIGHT_ON_PCT = "light_on_pct"
    const val KEY_LIGHT_OFF_PCT = "light_off_pct"
    const val KEY_AUTO_LIGHT = "auto_light_enable"
    const val KEY_BUZZER_ON_C = "buzzer_on_c"
    const val KEY_BUZZER_OFF_C = "buzzer_off_c"
    const val KEY_AUTO_BUZZER = "auto_buzzer_enable"
    const val KEY_TEMP_ALERT_C = "temp_alert_c"
    const val KEY_RAIN_PCT = "rain_pct"
    const val KEY_DOOR_LIGHT = "door_light_cascade"
    const val KEY_BUZZER_ACTIVE_LOW = "buzzer_active_low"

    // 数值型的几个
    val NUM_KEYS = listOf(
        KEY_LIGHT_ON_PCT to "光照下限 %",
        KEY_LIGHT_OFF_PCT to "光照上限 %",
        KEY_BUZZER_ON_C to "蜂鸣器上限 ℃",
        KEY_BUZZER_OFF_C to "蜂鸣器下限 ℃",
        KEY_TEMP_ALERT_C to "温度告警 ℃",
        KEY_RAIN_PCT to "雨滴阈值 %",
    )

    // 开关型的几个
    val BOOL_KEYS = listOf(
        KEY_AUTO_LIGHT to "光照联动",
        KEY_AUTO_BUZZER to "蜂鸣器联动",
        KEY_DOOR_LIGHT to "开门亮灯",
        KEY_BUZZER_ACTIVE_LOW to "蜂鸣器低电平",
    )

    // 固件指令短名
    val VOICE_SHORTCUTS = listOf(
        "query_temp" to "播报温度",
        "query_humi" to "播报湿度",
        "query_all" to "播报温湿度",
        "query_weather" to "播报天气",
        "query_light" to "播报光照",
        "query_status" to "播报状态",
    )

    // 拼开关指令
    fun cmd(dev: String, action: String): String = """{"dev":"$dev","action":"$action"}"""

    // 拼改阈值指令
    fun cmdConfigInt(key: String, value: Int): String =
        """{"dev":"config","action":"$key","value":$value}"""

    // 布尔要发 true/false，加引号就废了
    fun cmdConfigBool(key: String, value: Boolean): String =
        """{"dev":"config","action":"$key","value":$value}"""

    // 拼改模式指令
    fun cmdMode(auto: Boolean): String =
        """{"action":"mode","value":"${if (auto) "auto" else "manual"}"}"""

    // 拼静音指令
    fun cmdBuzzer(on: Boolean): String = """{"action":"buzzer","value":"${if (on) "on" else "off"}"}"""

    // 拼语音指令
    fun cmdVoice(action: String): String = """{"dev":"voice","action":"$action"}"""

    // 拼 WiFi 下发
    fun cmdWifi(ssid: String, pass: String): String {
        val o = org.json.JSONObject()
        o.put("ssid", ssid)
        o.put("pass", pass)
        return o.toString()
    }
}
