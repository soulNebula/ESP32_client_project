// c
// 开机启动与调试台
//
// 本工程是 ESP-IDF 版「ESP32-S3 智能家居（光影语音版）」的 Arduino 版。
// 串口、引脚、话题、JSON 字段、串口台命令都和 ESP-IDF 版一致，方便对照排查。

#include <string.h>
#include <stdlib.h>

#include "board.h"
#include "bsp_config.h"
#include "app_config.h"

#include "gpio_util.h"
#include "i2c_bus.h"
#include "led.h"
#include "servo.h"
#include "buzzer.h"
#include "sensor.h"
#include "key.h"
#include "voice.h"

#include "panel.h"
#include "display_view.h"

#include "device_model.h"
#include "app_cmd.h"
#include "automation.h"
#include "home_auto.h"
#include "net_link.h"
#include "ble_app.h"

static const char *TAG = "MAIN";

// 主循环节拍
#define APP_LOOP_TICK_MS_LOCAL APP_LOOP_TICK_MS

// 回调里别阻塞
static volatile bool s_state_dirty = false;

#if APP_USE_KEY
// KEY1 长按锁
static bool s_key1_long_done = false;

// KEY2 长按锁
static bool s_key2_long_done = false;
#endif

// 累积计时
static uint32_t s_sensor_acc_ms;
static uint32_t s_state_acc_ms;
static uint32_t s_led_acc_ms;
static uint32_t s_last_hb_ms;

// 设备状态变了
static void on_device_changed(device_id_t id, device_state_t st, ctrl_source_t src, void *user) {
    (void)st;
    (void)user;
    s_state_dirty = true;

    if (src != SRC_AUTO) {
        // 200ms 合并
        static uint32_t s_last_event_ms = 0;
        const uint32_t  now             = app_millis();
        if ((now - s_last_event_ms) >= 200) {
            s_last_event_ms = now;
            (void)net_link_publish_event("device", home_ctrl_dev_name(id));
        }
    }
}

// 语音转设备动作

// 字库外的字画框
static char     s_voice_cmd_cn[24]; // 识别到的指令
static char     s_voice_act_cn[28]; // 动作结果
static uint32_t s_voice_tick = 0;   // 事件计数

// NULL 只显示指令
static void voice_set_feedback(const char *cmd_cn, const char *act_cn) {
    snprintf(s_voice_cmd_cn, sizeof(s_voice_cmd_cn), "%s", (cmd_cn != NULL) ? cmd_cn : "");
    snprintf(s_voice_act_cn, sizeof(s_voice_act_cn), "%s", (act_cn != NULL) ? act_cn : "");
    s_voice_tick++;

    LOGI(TAG, "屏幕反馈: [%s] %s", s_voice_cmd_cn, s_voice_act_cn);
}

// 短名转中文名
static const char *voice_dev_cn(const char *dev) {
    if (dev == NULL) {
        return "";
    }
    if (strcmp(dev, "led_living") == 0) {
        return "客厅灯";
    }
    if (strcmp(dev, "led_kitchen") == 0) {
        return "厨房灯";
    }
    if (strcmp(dev, "led_bedroom") == 0) {
        return "卧室灯";
    }
    if (strcmp(dev, "led_bath") == 0) {
        return "浴室灯";
    }
    if (strcmp(dev, "all") == 0) {
        return "全部灯";
    }
    if (strcmp(dev, "door") == 0) {
        return "门";
    }
    if (strcmp(dev, "buzzer") == 0) {
        return "蜂鸣器";
    }
    return dev;
}

// 自动模式拒灯
static bool voice_light_blocked(void) {
    return auto_policy_is_auto();
}

// 与 ack 话术一致
#define AUTO_MODE_HINT_TEXT "自动模式下灯光由光照控制，请先切到手动模式"

// 全走指令路由
static void voice_on_cmd(voice_cmd_t cmd, void *user) {
    (void)user;

    const char *dev    = NULL;
    const char *action = NULL;

    switch (cmd) {
    // 单房间灯
    case VOICE_CMD_LED_LIVING_ON:
        dev    = "led_living";
        action = "on";
        break;
    case VOICE_CMD_LED_LIVING_OFF:
        dev    = "led_living";
        action = "off";
        break;
    case VOICE_CMD_LED_KITCHEN_ON:
        dev    = "led_kitchen";
        action = "on";
        break;
    case VOICE_CMD_LED_KITCHEN_OFF:
        dev    = "led_kitchen";
        action = "off";
        break;
    case VOICE_CMD_LED_BEDROOM_ON:
        dev    = "led_bedroom";
        action = "on";
        break;
    case VOICE_CMD_LED_BEDROOM_OFF:
        dev    = "led_bedroom";
        action = "off";
        break;
    case VOICE_CMD_LED_BATH_ON:
        dev    = "led_bath";
        action = "on";
        break;
    case VOICE_CMD_LED_BATH_OFF:
        dev    = "led_bath";
        action = "off";
        break;

    // 全灯不开门
    case VOICE_CMD_LED_ALL_ON:
        dev    = "all";
        action = "on";
        break;
    case VOICE_CMD_LED_ALL_OFF:
        dev    = "all";
        action = "off";
        break;

    // 开门联动客厅灯
    case VOICE_CMD_DOOR_OPEN:
        dev    = "door";
        action = "open";
        break;
    case VOICE_CMD_DOOR_CLOSE:
        dev    = "door";
        action = "close";
        break;

    // 模式切换
    case VOICE_CMD_MODE_AUTO:
        dev    = "mode";
        action = "auto";
        break;
    case VOICE_CMD_MODE_MANUAL:
        dev    = "mode";
        action = "manual";
        break;

    // off 是静音锁
    case VOICE_CMD_BUZZER_OFF:
        dev    = "buzzer";
        action = "off";
        break;
    case VOICE_CMD_BUZZER_ON:
        dev    = "buzzer";
        action = "on";
        break;

    // 查询单独处理
    case VOICE_CMD_QUERY_TEMP:
    case VOICE_CMD_QUERY_HUMI:
    case VOICE_CMD_QUERY_ALL:
    case VOICE_CMD_QUERY_WEATHER:
    case VOICE_CMD_QUERY_LIGHT:
    case VOICE_CMD_QUERY_STATUS:
    default:
        break;
    }

    if (dev != NULL) {
        char            detail[HOME_CMD_DETAIL_LEN] = {0};
        const app_err_t err = home_command_execute(dev, action, -1, SRC_VOICE, detail);

        char act[28];
        if (err == APP_OK) {
            if (strcmp(dev, "mode") == 0) {
                snprintf(act, sizeof(act), "已切换到%s模式", (strcmp(action, "auto") == 0) ? "自动" : "手动");
            } else if (strcmp(dev, "buzzer") == 0) {
                snprintf(act, sizeof(act), "蜂鸣器 已%s", (strcmp(action, "off") == 0) ? "静音" : "打开");
            } else {
                snprintf(act, sizeof(act), "%s 已%s", voice_dev_cn(dev),
                         (strcmp(action, "off") == 0 || strcmp(action, "close") == 0) ? "关闭" : "打开");
            }
        } else if (err == APP_ERR_INVALID_STATE && voice_light_blocked()) {
            snprintf(act, sizeof(act), "未执行: 自动模式");
        } else {
            snprintf(act, sizeof(act), "执行失败");
        }
        voice_set_feedback(bsp_voice_cmd_cn(cmd), act);

        if (err == APP_ERR_INVALID_STATE && voice_light_blocked()) {
            // 必须回一句
            (void)bsp_voice_speak(AUTO_MODE_HINT_TEXT);
        } else if (err != APP_OK && err != APP_ERR_NOT_FOUND) {
            (void)bsp_voice_speak("操作没有成功，请检查设备");
            LOGW(TAG, "指令 %s/%s 执行失败: %s", dev, action, detail[0] ? detail : home_command_err_text(err));
        }
        return;
    }

    sensor_snapshot_t sn;
    bsp_sensors_get(&sn);

    switch (cmd) {
    case VOICE_CMD_QUERY_TEMP:
        (void)bsp_voice_speak_temp(sn.temperature, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_HUMI:
        (void)bsp_voice_speak_humi(sn.humidity, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_ALL:
        (void)bsp_voice_speak_temp_humi(sn.temperature, sn.humidity, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_WEATHER:
        (void)bsp_voice_speak_weather(sn.raining, sn.rain_valid);
        break;
    case VOICE_CMD_QUERY_LIGHT:
        (void)bsp_voice_speak_light(sn.light_pct, sn.light_valid);
        break;
    case VOICE_CMD_QUERY_STATUS:
        // 温湿度加天气
        (void)bsp_voice_speak_temp_humi(sn.temperature, sn.humidity, sn.th_valid);
        (void)bsp_voice_speak_weather(sn.raining, sn.rain_valid);
        break;
    default:
        LOGD(TAG, "指令 %d 无需动作", (int)cmd);
        break;
    }

    if (cmd == VOICE_CMD_QUERY_ALL || cmd == VOICE_CMD_QUERY_STATUS) {
        char act[28];
        if (sn.th_valid) {
            snprintf(act, sizeof(act), "%.1f度 %.0f%%%s", (double)sn.temperature, (double)sn.humidity,
                     (cmd == VOICE_CMD_QUERY_STATUS) ? (sn.raining ? " 有雨" : " 无雨") : "");
        } else {
            snprintf(act, sizeof(act), "传感器未接");
        }
        voice_set_feedback(bsp_voice_cmd_cn(cmd), act);
    } else {
        voice_set_feedback(bsp_voice_cmd_cn(cmd), bsp_voice_last_spoken());
    }
}

// 手机语音同一链路
static app_err_t on_injected_voice(const char *token) {
    const voice_cmd_t cmd = bsp_voice_token_to_cmd(token);
    if (cmd == VOICE_CMD_NONE) {
        return APP_ERR_NOT_FOUND;
    }
    voice_on_cmd(cmd, NULL);
    return APP_OK;
}

// CMD 才动作
static void on_voice_event(voice_event_t ev, voice_cmd_t cmd, const char *raw, void *user) {
    (void)user;

    switch (ev) {
    case VOICE_EVENT_CMD:
        voice_on_cmd(cmd, NULL);
        break;

    case VOICE_EVENT_WAKE:
        // 唤醒提示
        bsp_status_led_show(BSP_STATUS_ONLINE);
        LOGI(TAG, "语音助手被唤醒");
        // 逗号用半角
        voice_set_feedback("已唤醒", "在听, 请说指令");
        break;

    case VOICE_EVENT_RAW:
        // 不认识只记日志
        LOGD(TAG, "语音原始内容: %s", raw != NULL ? raw : "(null)");
        break;

    default:
        break;
    }
}

#if APP_USE_KEY

// 按键回调要轻量
static void on_key_event(bsp_key_id_t key, bsp_key_event_t event, void *user) {
    (void)user;
    char detail[HOME_CMD_DETAIL_LEN] = {0};

    if (key == BSP_KEY_1) {
        if (event == BSP_KEY_EVENT_SHORT) {
            const char *want = auto_policy_is_auto() ? "manual" : "auto";
            (void)home_command_execute("mode", want, -1, SRC_BUTTON, detail);
            (void)bsp_voice_speak_mode(!auto_policy_is_auto());
            (void)net_link_publish_event("key", "mode");
        } else if (event == BSP_KEY_EVENT_LONG) {
            // 长按重复上报
            if (!s_key1_long_done) {
                s_key1_long_done = true;
                (void)auto_policy_reset_defaults();
                (void)bsp_voice_speak("已恢复默认设置");
                LOGW(TAG, "KEY1 长按：阈值已恢复默认值");
            }
        } else {
            // 松开复位锁
            s_key1_long_done = false;
        }
        return;
    }

    if (key == BSP_KEY_2) {
        if (event == BSP_KEY_EVENT_SHORT) {
            (void)home_command_execute("all", "toggle", -1, SRC_BUTTON, detail);
            (void)net_link_publish_event("key", "all_toggle");
        } else if (event == BSP_KEY_EVENT_LONG) {
            // 长按只翻转一次
            if (!s_key2_long_done) {
                s_key2_long_done = true;
                (void)home_command_execute("door", "toggle", -1, SRC_BUTTON, detail);
                (void)net_link_publish_event("key", "door_toggle");
            }
        } else {
            // 松开复位锁
            s_key2_long_done = false;
        }
    }
}

#endif

// 组屏幕视图
static void build_view_input(const sensor_snapshot_t *sn, display_view_input_t *in) {
    memset(in, 0, sizeof(*in));

    in->temperature = sn->temperature;
    in->humidity    = sn->humidity;
    in->th_valid    = sn->th_valid;

    in->raining    = sn->raining;
    in->rain_pct   = sn->rain_pct;
    in->rain_valid = sn->rain_valid;

    in->light_pct    = sn->light_pct;
    in->light_lux    = sn->light_lux;
    in->light_valid  = sn->light_valid;
    in->light_is_lux = bsp_sensors_light_is_lux();
    in->auto_mode    = auto_policy_is_auto();

    in->light_on[VIEW_LIGHT_LIVING]  = home_ctrl_is_on(DEV_LED_LIVING);
    in->light_on[VIEW_LIGHT_KITCHEN] = home_ctrl_is_on(DEV_LED_KITCHEN);
    in->light_on[VIEW_LIGHT_BEDROOM] = home_ctrl_is_on(DEV_LED_BEDROOM);
    in->light_on[VIEW_LIGHT_BATH]    = home_ctrl_is_on(DEV_LED_BATH);
    in->door_open                    = home_ctrl_is_on(DEV_DOOR);
    in->buzzer_on                    = home_ctrl_is_on(DEV_BUZZER);
    in->buzzer_muted                 = auto_policy_buzzer_muted();

    in->wifi_ok = net_link_wifi_ok();
    in->mqtt_ok = net_link_mqtt_ok();
    in->rssi    = net_link_rssi();
    snprintf(in->ip, sizeof(in->ip), "%s", net_link_ip());

    // 阈值
    const auto_cfg_t *cfg = auto_policy_cfg();
    if (cfg != NULL) {
        in->light_on_pct  = cfg->light_on_pct;
        in->light_off_pct = cfg->light_off_pct;
        in->buzzer_on_c   = cfg->buzzer_on_c;
        in->temp_alert_c  = cfg->temp_alert_c;
    } else {
        in->light_on_pct  = BSP_DEF_LIGHT_ON_PCT;
        in->light_off_pct = BSP_DEF_LIGHT_OFF_PCT;
        in->buzzer_on_c   = BSP_DEF_BUZZER_ON_C;
        in->temp_alert_c  = BSP_DEF_TEMP_ALERT_C;
    }

    snprintf(in->voice_line1, sizeof(in->voice_line1), "%s", s_voice_cmd_cn);
    snprintf(in->voice_line2, sizeof(in->voice_line2), "%s", s_voice_act_cn);
    in->voice_tick = s_voice_tick;
}

// 任一通道通就算在线
static bool link_online(void) {
    return net_link_mqtt_ok() || ble_app_is_connected();
}

// 网络转状态灯
static int status_led_code(void) {
    if (link_online()) {
        // 报警优先
        if (home_ctrl_is_on(DEV_BUZZER)) {
            return BSP_STATUS_ALERT;
        }
        return BSP_STATUS_ONLINE;
    }
    return BSP_STATUS_NO_WIFI;
}

// 主循环体
static void app_loop_once(uint32_t tick_ms) {
    // 1. 先跑联动：它可能改设备状态
    sensor_snapshot_t sn;
    bsp_sensors_get(&sn);
    home_auto_tick(&sn);

    // 2. 再推屏：拿到的是本轮最新状态
    display_view_input_t view;
    build_view_input(&sn, &view);
    display_view_update(&view);

    // 3. 周期上报
    s_sensor_acc_ms += tick_ms;
    if (s_sensor_acc_ms >= APP_MQTT_PUBLISH_SENSOR_MS) {
        s_sensor_acc_ms = 0;
        if (link_online()) {
            (void)net_link_publish_sensor(&sn);
            (void)net_link_publish_weather(&sn);
        }
    }

    // 变化立刻上报
    // 最多 200ms 合并一次，防抖
    s_state_acc_ms += tick_ms;
    if (s_state_dirty && (s_state_acc_ms >= APP_STATE_COALESCE_MS || !link_online())) {
        if (link_online()) {
            (void)net_link_publish_state();
            s_state_acc_ms = 0;
        }
        s_state_dirty = false;
    }

    // 4. 状态灯很亮，不必跟 500ms
    s_led_acc_ms += tick_ms;
    if (s_led_acc_ms >= APP_STATUS_LED_PERIOD_MS) {
        s_led_acc_ms = 0;
        bsp_status_led_show(status_led_code());
    }
}

// 每轮都要跑的
static void app_service(void) {
    bsp_sensors_tick();
    bsp_key_tick();
    bsp_buzzer_tick();

    net_link_tick();
    display_view_tick();
}

// 串口调试台

// 英文化开关
static void console_print_help(void) {
#if APP_CONSOLE_ASCII
    LOGI(TAG, "==================== console ====================");
    LOGI(TAG, "  help                      show this");
    LOGI(TAG, "  status                    devices + sensors + net + mode + thresholds");
    LOGI(TAG, "  on <dev> / off <dev>      switch a device");
    LOGI(TAG, "  toggle <dev>              flip a device");
    LOGI(TAG, "  set <dev> <0-100>         light=brightness, door=open pct (>=50 = open)");
    LOGI(TAG, "  all on | all off          4 lights on / off (never touches the door)");
    LOGI(TAG, "  open door | close door    open door (also lights the living room) / close");
    LOGI(TAG, "  mode auto | mode manual   switch mode (saved to NVS)");
    LOGI(TAG, "  buzzer on|off|mute        buzzer on / off / mute-lock");
    LOGI(TAG, "  cfg                       print thresholds");
    LOGI(TAG, "  cfg <key> <value>         set a threshold, e.g. cfg buzzer_on_c 26");
    LOGI(TAG, "  cfg save | cfg reset      save / restore default thresholds");
    LOGI(TAG, "  say <cmd>                 inject a voice command (full voice path)");
    LOGI(TAG, "  help-voice                list every voice command");
    LOGI(TAG, "  sensor                    sample sensors once now");
    LOGI(TAG, "  screen                    force a redraw");
    LOGI(TAG, "  screen-test               panel self-test");
    LOGI(TAG, "  font-test                 font size comparison");
    LOGI(TAG, "  panel-retry               re-init the panel (no reboot needed)");
    LOGI(TAG, "  i2c-scan                  scan the I2C bus");
    LOGI(TAG, "  buzzer-test               buzzer polarity self-test");
    LOGI(TAG, "  hw                        hardware diagnostics");
    LOGI(TAG, "  wifi | mqtt               print WiFi / MQTT state and topics");
    LOGI(TAG, "  ble                       print the BLE name and connection state");
    LOGI(TAG, "  log on | log off          verbose logging");
    LOGI(TAG, "  reboot                    restart the board");
    LOGI(TAG, "  <dev>: led_living led_kitchen led_bedroom led_bath door");
    LOGI(TAG, "=================================================");
#else
    LOGI(TAG, "==================== 串口调试台命令 ====================");
    LOGI(TAG, "  help                      显示本帮助");
    LOGI(TAG, "  status                    设备 + 传感器 + 网络 + 模式 + 阈值");
    LOGI(TAG, "  on <dev> / off <dev>      开/关设备");
    LOGI(TAG, "  toggle <dev>              翻转设备");
    LOGI(TAG, "  set <dev> <0-100>         灯=亮度，门=开合度(>=50 视为开)");
    LOGI(TAG, "  all on | all off          4 路灯全亮 / 全灭（绝不动门）");
    LOGI(TAG, "  open door | close door    开门（会联动亮客厅灯）/ 关门");
    LOGI(TAG, "  mode auto | mode manual   切换手动/自动模式（存 NVS）");
    LOGI(TAG, "  buzzer on|off|mute        蜂鸣器开 / 关 / 静音锁");
    LOGI(TAG, "  cfg                       打印当前阈值");
    LOGI(TAG, "  cfg <key> <value>         改阈值，例如 cfg buzzer_on_c 26");
    LOGI(TAG, "  cfg save | cfg reset      保存 / 恢复默认阈值");
    LOGI(TAG, "  say <cmd>                 ★ 注入一条语音指令，走完整语音链路");
    LOGI(TAG, "  help-voice                列出全部语音指令短名 + 中文说法");
    LOGI(TAG, "  sensor                    立刻采一次传感器并打印");
    LOGI(TAG, "  screen                    强制重画一屏（调屏排版用）");
    LOGI(TAG, "  screen-test               屏幕自检：纯色轮播 + 边框/交叉线");
    LOGI(TAG, "  font-test                 ★字号对照：小字 vs 放大2倍 并排显示");
    LOGI(TAG, "  panel-retry               ★重跑屏幕初始化（改完接线不用重新烧录）");
    LOGI(TAG, "  i2c-scan                  ★实时扫 I2C：查屏/传感器认不到时边插边看");
    LOGI(TAG, "  buzzer-test               ★蜂鸣器极性自检：听哪一段安静，据此定极性");
    LOGI(TAG, "  hw                        硬件诊断：引脚实际电平 + 关键外设状态");
    LOGI(TAG, "  wifi | mqtt               打印 WiFi / MQTT 连接状态与 topic");
    LOGI(TAG, "  ble                       打印蓝牙设备名与连接状态");
    LOGI(TAG, "  log on | log off          打开 / 关闭详细日志");
    LOGI(TAG, "  reboot                    重启");
    LOGI(TAG, "  <dev> 取值: led_living led_kitchen led_bedroom led_bath door");
    LOGI(TAG, "======================================================");
#endif
}

static void console_print_status(void) {
    LOGI(TAG, "---------- 设备 ----------");
    home_ctrl_dump();

    LOGI(TAG, "---------- 传感器 ----------");
    sensor_snapshot_t sn;
    bsp_sensors_get(&sn);
    LOGI(TAG, "  温度 %.1fC (valid=%d)  湿度 %.1f%% (valid=%d)", sn.temperature, (int)sn.th_valid, sn.humidity,
         (int)sn.th_valid);
    LOGI(TAG, "  光照 %.1f%% (%.1flux, valid=%d, %s)", sn.light_pct, sn.light_lux, (int)sn.light_valid,
         bsp_sensors_light_is_lux() ? "BH1750" : "光敏电阻");
    LOGI(TAG, "  雨滴 %.1f%% 天气=%s (valid=%d, 阈值 %.0f%%)", sn.rain_pct,
         sn.raining ? "Raining!" : "Sunny", (int)sn.rain_valid, bsp_sensors_get_rain_threshold());

    LOGI(TAG, "---------- 网络 ----------");
    net_link_dump();

    LOGI(TAG, "---------- 自动化 ----------");
    auto_policy_dump();
    LOGI(TAG, "  蜂鸣器静音锁 = %s", auto_policy_buzzer_muted() ? "生效中" : "未生效");
    LOGI(TAG, "----------------------------");
}

// 硬件诊断
static void console_print_hw(void) {
    LOGI(TAG, "---------- 硬件诊断 ----------");
    LOGI(TAG, "模块自检表：");
    LOGI(TAG,
         "  状态灯=%-3s I2C=%-3s 灯=%-3s 舵机=%-3s 蜂鸣器=%-3s 传感器=%-3s 屏=%-3s 按键=%-3s 语音=%-3s",
         board_module_ok(BOARD_MOD_STATUS_LED) ? "OK" : "NO", board_module_ok(BOARD_MOD_I2C) ? "OK" : "NO",
         board_module_ok(BOARD_MOD_LED) ? "OK" : "NO", board_module_ok(BOARD_MOD_SERVO) ? "OK" : "NO",
         board_module_ok(BOARD_MOD_BUZZER) ? "OK" : "NO", board_module_ok(BOARD_MOD_SENSOR) ? "OK" : "NO",
         board_module_ok(BOARD_MOD_PANEL) ? "OK" : "NO", board_module_ok(BOARD_MOD_KEY) ? "OK" : "NO",
         board_module_ok(BOARD_MOD_VOICE) ? "OK" : "NO");

    LOGI(TAG, "显示屏   = %s，走 I2C（无背光引脚）", BSP_PANEL_NAME);
    LOGI(TAG, "          SDA=GPIO%d  SCL=GPIO%d  地址=0x%02X（备选 0x%02X）", BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO,
         BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT);
    LOGI(TAG, "          ⚠ OLED 自己发光、【没有 BLK 背光脚】——");
    LOGI(TAG, "            屏全黑就两条路：I2C 没通（敲 panel-retry 看日志）或地址跳线不对。");

    LOGI(TAG, "蜂鸣器   = GPIO%d，配置为【%s】，逻辑电平=%d（0 = 应为静音）", BSP_BUZZER_GPIO,
         BSP_BUZZER_ACTIVE_LOW ? "低电平触发" : "高电平触发", bsp_gpio_read(BSP_BUZZER_GPIO));
    LOGI(TAG, "        ⚠ 如果上面电平=0 而蜂鸣器【仍然一直响】：");
    LOGI(TAG, "          说明你的模块是【低电平触发型】——");
    LOGI(TAG, "          串口敲： cfg buzzer_active_low 1（或改 bsp_config.h 的 BSP_BUZZER_ACTIVE_LOW）");

#if APP_USE_KEY
    LOGI(TAG, "按键     KEY1=GPIO%d 电平=%d  KEY2=GPIO%d 电平=%d（0=按下）", BSP_KEY_GPIO_KEY1,
         bsp_gpio_read(BSP_KEY_GPIO_KEY1), BSP_KEY_GPIO_KEY2, bsp_gpio_read(BSP_KEY_GPIO_KEY2));
#else
    LOGI(TAG, "按键     未启用（这块板子没有按键，控制走手机 App / 网页面板 / 串口）");
#endif

    LOGI(TAG, "语音     板载识别(ESP_SR) = %s，唤醒词「%s」",
         bsp_voice_native_ready() ? "已启动" : "未启动", bsp_voice_wake_word());
    if (!bsp_voice_native_ready()) {
        LOGI(TAG, "         未启动就喊不动。两条路：");
        LOGI(TAG, "         ① Tools → Partition Scheme 选 \"ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)\"，重烧");
        LOGI(TAG, "         ② 先用手机 App / 串口 say <短名> —— 走的是同一条下游，效果一样");
    }
    LOGI(TAG, "         麦克风 INMP441: SCK=GPIO%d WS=GPIO%d SD=GPIO%d @%dHz（L/R 接 GND）",
         BSP_I2S_MIC_SCK_GPIO, BSP_I2S_MIC_WS_GPIO, BSP_I2S_MIC_SD_GPIO, APP_VOICE_SAMPLE_RATE);
    LOGI(TAG, "-------------------------------");
}

// 执行一行命令
static void console_run_line(char *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }

    char detail[HOME_CMD_DETAIL_LEN];
    detail[0] = '\0';

    // help
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        console_print_help();
        return;
    }
    if (strcmp(line, "help-voice") == 0) {
        bsp_voice_dump_cmds();
        return;
    }
    if (strcmp(line, "status") == 0) {
        console_print_status();
        return;
    }
    if (strcmp(line, "sensor") == 0) {
        bsp_sensors_sample_now();
        sensor_snapshot_t sn;
        bsp_sensors_get(&sn);
        LOGI(TAG, "采样: %.1fC / %.1f%% / 光照 %.1f%% / 雨滴 %.1f%% (%s)", sn.temperature, sn.humidity, sn.light_pct,
             sn.rain_pct, sn.raining ? "Raining!" : "Sunny");
        return;
    }
    if (strcmp(line, "screen") == 0) {
        display_view_force_redraw();
        LOGI(TAG, "已请求重画屏幕");
        return;
    }
    // 屏幕自检
    if (strcmp(line, "screen-test") == 0 || strncmp(line, "screen-test ", 12) == 0) {
        int hold = 0;
        if (strlen(line) > 12) {
            hold = atoi(line + 12);
        }
        LOGI(TAG, "开始屏幕自检（每屏 %d ms，0=默认）...", hold);
        panel_selftest(hold);
        display_view_force_redraw(); // 画回业务界面
        return;
    }
    // 字号对照
    if (strcmp(line, "font-test") == 0) {
        LOGI(TAG, "字号对照测试：第 1/3 行是小字(8x8)，第 2 行是放大 2 倍(=16x16)，第 4 行是大字 ASCII");
        panel_clear(PANEL_BLACK);
        // 小字
        panel_draw_text_cn(0, 0, "温湿 28.7C", PANEL_WHITE, PANEL_TRANSPARENT);
        // 放大 2 倍
        panel_draw_text_cn_scaled(0, 12, "温湿 28.7C", PANEL_WHITE, PANEL_TRANSPARENT, 2);
        // ASCII 大字
        panel_draw_text(0, 40, "28.7C abc", PANEL_WHITE, PANEL_TRANSPARENT);
        panel_flush_all();
        LOGI(TAG, "★ 判读：若第 1 行明显【小于】第 2 行 → 小字版生效(正常)");
        delay(6000);
        display_view_force_redraw(); // 画回业务界面
        return;
    }
    // 重跑屏初始化
    if (strcmp(line, "panel-retry") == 0) {
        LOGW(TAG, "重跑屏幕初始化（复用总线，只重发初始化序列）...");
        const app_err_t perr = display_view_retry_panel();
        if (perr != APP_OK) {
            LOGE(TAG, "屏幕重新初始化失败: %s", app_err_text(perr));
            return;
        }
        LOGI(TAG, "屏幕重新初始化成功，接着跑一次自检");
        panel_selftest(0);
        display_view_force_redraw();
        return;
    }
    if (strcmp(line, "buzzer-test") == 0) {
        (void)bsp_buzzer_polarity_test();
        return;
    }
    // 实时扫 I2C
    if (strcmp(line, "i2c-scan") == 0) {
        uint8_t   found[BSP_I2C_SCAN_MAX];
        const int hits = bsp_i2c_scan(found, (int)sizeof(found));

        LOGI(TAG,
             "本工程期望的地址：0x38=AHT20 温湿度  0x3C/0x3D=SSD1306 屏  "
             "0x23=BH1750 光照  0x44=SHT30 温湿度");
        bool panel_here = false;
        for (int i = 0; i < hits; i++) {
            if (found[i] == BSP_I2C_ADDR_SSD1306 || found[i] == BSP_I2C_ADDR_SSD1306_ALT) {
                panel_here = true;
            }
        }
        if (panel_here) {
            LOGI(TAG,
                 "★ 屏在总线上（0x3C/0x3D 有应答）→ 接线没问题，"
                 "若屏仍全黑请敲 panel-retry 重跑初始化；还黑就查模块是否 SH1106（需改列偏移）");
        } else {
            LOGW(TAG, "屏【不在】总线上（0x3C/0x3D 都没应答）—— 按顺序查：");
            LOGW(TAG, "  ① 屏的 VCC 对 GND 是否有 3.3V（模块有 LDO 才能接 5V，裸屏必须 3.3V）");
            LOGW(TAG, "  ② SDA→GPIO%d / SCL→GPIO%d 是否松、是否插在了不同的面包板排上", BSP_I2C_SDA_GPIO,
                 BSP_I2C_SCL_GPIO);
            LOGW(TAG, "  ③ 快速判别法：把屏的 SDA/SCL 两根线拔下来，插到 AHT20(0x38) 正在用的"
                      "那两排孔上，再敲一次 i2c-scan ——");
            LOGW(TAG, "     出现了 0x3C → 屏和模块都好，坏的是它原来那两根线/两个孔；");
            LOGW(TAG, "     仍然没有 → 供电或模块本身的问题（与总线、与固件无关）。");
        }
        return;
    }
    // 硬件诊断
    if (strcmp(line, "hw") == 0) {
        console_print_hw();
        return;
    }
    if (strcmp(line, "wifi") == 0 || strcmp(line, "mqtt") == 0) {
        net_link_dump();
        return;
    }
    if (strcmp(line, "ble") == 0) {
        LOGI(TAG, "蓝牙设备名 : %s", ble_app_name());
        LOGI(TAG, "手机连接   : %s", ble_app_is_connected() ? "已连接" : "未连接");
        LOGI(TAG, "Service    : 5a1c0001-1e2f-4a3b-9c4d-5e6f708192a3");
        LOGI(TAG, "RX(写)     : 5a1c0002-1e2f-4a3b-9c4d-5e6f708192a3");
        LOGI(TAG, "TX(通知)   : 5a1c0003-1e2f-4a3b-9c4d-5e6f708192a3");
        return;
    }
    if (strcmp(line, "log on") == 0) {
        app_log_set_verbose(true);
        return;
    }
    if (strcmp(line, "log off") == 0) {
        app_log_set_verbose(false);
        LOGI(TAG, "详细日志已关闭");
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        LOGW(TAG, "重启中 ...");
        delay(100);
        ESP.restart();
        return;
    }

    // cfg
    if (strncmp(line, "cfg", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        char *rest = line + 3;
        while (*rest == ' ') {
            rest++;
        }
        if (*rest == '\0') {
            auto_policy_dump();
            return;
        }
        if (strcmp(rest, "save") == 0) {
            LOGI(TAG, "cfg save -> %s", app_err_text(auto_policy_save()));
            return;
        }
        if (strcmp(rest, "reset") == 0) {
            LOGI(TAG, "cfg reset -> %s", app_err_text(auto_policy_reset_defaults()));
            return;
        }
        // 值有字符串，不走路由
        char *key = rest;
        char *val = strchr(rest, ' ');
        if (val == NULL) {
            LOGW(TAG, "用法: cfg <key> <value>  或  cfg / cfg save / cfg reset");
            return;
        }
        *val++ = '\0';
        while (*val == ' ') {
            val++;
        }

        const app_err_t err = auto_policy_set_threshold(key, val);
        if (err == APP_OK) {
            LOGI(TAG, "cfg %s = %s", key, val);
        } else {
            LOGW(TAG, "cfg %s %s 被拒: %s", key, val, app_err_text(err));
            LOGW(TAG, "  提示：改上下限时先发 *_on 再发 *_off；键名见 automation.h");
        }
        return;
    }

    // say 注入
    if (strncmp(line, "say ", 4) == 0) {
        const char *token = line + 4;
        while (*token == ' ') {
            token++;
        }
        const app_err_t err = bsp_voice_inject(token);
        if (err != APP_OK) {
            LOGW(TAG, "不认识的语音指令 \"%s\"（输入 help-voice 看全部）", token);
        }
        return;
    }

    if (strncmp(line, "all ", 4) == 0) {
        const char     *act = line + 4;
        const app_err_t err = home_command_execute("all", act, -1, SRC_CONSOLE, detail);
        LOGI(TAG, "all %s -> %s %s", act, app_err_text(err), detail);
        return;
    }

    char      verb[16] = {0};
    char      dev[32]  = {0};
    int       pct      = -1;
    const int got      = sscanf(line, "%15s %31s %d", verb, dev, &pct);

    if (got >= 2) {
        // 两缓冲必须独立
        char        dev_final[32];
        const char *action_final = NULL;

        snprintf(dev_final, sizeof(dev_final), "%s", dev);

        if (strcmp(verb, "on") == 0) {
            action_final = "on";
        } else if (strcmp(verb, "off") == 0) {
            action_final = "off";
        } else if (strcmp(verb, "toggle") == 0) {
            action_final = "toggle";
        } else if (strcmp(verb, "set") == 0) {
            action_final = "set";
        } else if (strcmp(verb, "open") == 0) {
            // open 当开
            action_final = "open";
        } else if (strcmp(verb, "close") == 0) {
            action_final = "close";
        } else if (strcmp(verb, "mode") == 0) {
            static char mode_val[16];
            snprintf(mode_val, sizeof(mode_val), "%s", dev);
            snprintf(dev_final, sizeof(dev_final), "%s", "mode");
            action_final = mode_val;
        } else if (strcmp(verb, "buzzer") == 0) {
            static char buzzer_val[16];
            snprintf(buzzer_val, sizeof(buzzer_val), "%s", dev);
            snprintf(dev_final, sizeof(dev_final), "%s", "buzzer");
            action_final = buzzer_val;
        } else {
            LOGW(TAG, "未知命令 \"%s\"（输入 help 看全部）", verb);
            return;
        }

        const app_err_t err = home_command_execute(dev_final, action_final, pct, SRC_CONSOLE, detail);
        if (err == APP_OK) {
            LOGI(TAG, "%s %s %s -> OK", verb, dev_final, action_final);
        } else {
            LOGW(TAG, "%s %s %s -> %s（%s）", verb, dev_final, action_final, app_err_text(err),
                 detail[0] ? detail : home_command_err_text(err));
        }
        return;
    }

    LOGW(TAG, "未知命令 \"%s\"（输入 help 看全部）", line);
}

// 串口台攒一行
static void console_poll(void) {
    static char   line[APP_CONSOLE_LINE_MAX];
    static size_t len = 0;

    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) {
            break;
        }

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                console_run_line(line);
                len = 0;
            }
            continue;
        }

        // 退格
        if (c == 0x08 || c == 0x7F) {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (len < APP_CONSOLE_LINE_MAX - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;
            LOGW(TAG, "命令太长，已丢弃（上限 %d 字节）", APP_CONSOLE_LINE_MAX - 1);
        }
    }
}

// 应用入口
void setup(void) {
    Serial.begin(115200);
    delay(200);

    LOGI(TAG, " ");
    LOGI(TAG, "###########################################################");
    LOGI(TAG, "#  ESP32-S3 智能家居（光影语音版）—— Arduino 版");
    LOGI(TAG, "#  build %s %s", __DATE__, __TIME__);
    LOGI(TAG, "###########################################################");

    // 缺配件只告警
    board_dump_pinmap();
    (void)board_init();

    // 有依赖顺序
    (void)home_ctrl_init();
    (void)home_auto_init();

    (void)home_ctrl_register_cb(on_device_changed, NULL);

    // 先推一帧
#if APP_USE_PANEL
    if (display_view_init() == APP_OK) {
        sensor_snapshot_t   sn0;
        display_view_input_t v0;
        bsp_sensors_get(&sn0);
        build_view_input(&sn0, &v0);
        display_view_update(&v0);
        display_view_force_redraw();
    } else {
        LOGW(TAG, "屏幕不可用 —— 其余功能照常（配件没到也能跑）");
    }
#else
    LOGW(TAG, "屏幕已被 APP_USE_PANEL=0 关掉");
#endif

    // 无按键不注册
#if APP_USE_KEY
    (void)bsp_key_register_cb(BSP_KEY_1, on_key_event, NULL);
    (void)bsp_key_register_cb(BSP_KEY_2, on_key_event, NULL);
    LOGI(TAG, "按键已启用：KEY1(短按切模式/长按恢复阈值) KEY2(短按全灯/长按开关门)");
#else
    LOGI(TAG, "无按键板：控制入口 = 手机 App(蓝牙) + 网页面板(MQTT) + 串口调试台（APP_USE_KEY=0）");
#endif

    // 语音走同一入口
    (void)bsp_voice_register_cb(on_voice_event, NULL);
    home_command_set_voice_handler(on_injected_voice);

    // 传感器自采样
    (void)bsp_sensors_start_auto();

    // 联网不阻塞
    (void)net_link_init();

    // 蓝牙晚一步起
    // 等 UID 算好
    // 起不来也只告警
#if APP_USE_BLE
    {
        const app_err_t ble_err = ble_app_start();
        if (ble_err == APP_OK) {
            LOGI(TAG, "蓝牙控制通道已启动（手机 App 直连，和 MQTT / 网页并行）");
        } else {
            LOGW(TAG, "蓝牙没起来：%s —— 手机直连不可用，WiFi/MQTT/网页面板照常运行", app_err_text(ble_err));
        }
    }
#else
    LOGW(TAG, "蓝牙已被 APP_USE_BLE=0 关掉，手机 App 直连不可用");
#endif

    LOGI(TAG, "串口调试台已就绪 —— 输入 help 查看命令");
    LOGI(TAG, "system ready. free heap: %lu bytes", (unsigned long)ESP.getFreeHeap());
}

void loop(void) {
    // 到点才跑主循环体，其余时间做服务
    static uint32_t s_last_loop_ms = 0;

    console_poll();
    app_service();

    const uint32_t now = app_millis();
    if ((now - s_last_loop_ms) >= APP_LOOP_TICK_MS_LOCAL) {
        const uint32_t elapsed  = now - s_last_loop_ms;
        s_last_loop_ms          = now;
        app_loop_once(elapsed);
    }

    // 心跳
    if ((now - s_last_hb_ms) >= 60000u) {
        s_last_hb_ms = now;
        LOGD(TAG, "心跳: 空闲堆 %lu 字节", (unsigned long)ESP.getFreeHeap());
    }

    // 让出 CPU，别把看门狗喂饿
    delay(5);
}
