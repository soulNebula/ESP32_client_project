// Kotlin
// JSON 解析小工具（只包 org.json）
package com.smarthome.ble.data

import org.json.JSONObject

object Json {

    // 整段解成对象
    fun parseObject(text: String): JSONObject? = try {
        JSONObject(text)
    } catch (e: Exception) {
        null
    }

    // 缺字段用默认值
    fun bool(o: JSONObject?, key: String, def: Boolean = false): Boolean =
        o?.optBoolean(key, def) ?: def

    // 取整数
    fun int(o: JSONObject?, key: String, def: Int = 0): Int = o?.optInt(key, def) ?: def

    // 取小数
    fun dbl(o: JSONObject?, key: String, def: Double = 0.0): Double = o?.optDouble(key, def) ?: def

    // 取文字
    fun str(o: JSONObject?, key: String, def: String = ""): String =
        o?.optString(key, def) ?: def

    // 取里层对象
    fun obj(o: JSONObject?, key: String): JSONObject? = o?.optJSONObject(key)

    // 整数值不带小数点
    fun numText(v: Double): String =
        if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()
}
