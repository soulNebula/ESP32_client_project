// c
// 开机启动与调试台

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "board_config.h"
#include "gpio_util.h"
// i2c-scan 用
#include "i2c_bus.h"

#include "key.h"
// 查蜂鸣器极性
#include "buzzer.h"

#include "sensor.h"

#include "voice.h"

#include "display_view.h"
// 屏幕自检
#include "panel.h"

#include "device_model.h"
#include "app_cmd.h"

#include "home_auto.h"
#include "automation.h"

#include "net_link.h"
// 蓝牙控制通道
#include "ble_app.h"

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
#include "driver/uart.h"
// 防隐藏依赖
#include "driver/gpio.h"
#endif

static const char *TAG = "MAIN";

// 主循环节拍
#define APP_LOOP_TICK_MS 500

// 状态灯节流
#define STATUS_LED_PERIOD_MS 1000

// 回调里别阻塞
static volatile bool s_state_dirty = false;

// 启动时刻 ms
static int64_t s_boot_ms = 0;

#if CONFIG_APP_KEY_ENABLE
// KEY1 长按锁
static bool s_key1_long_done = false;

// KEY2 长按锁
static bool s_key2_long_done = false;
// 按键开关收尾
#endif

static void on_device_changed(device_id_t id, device_state_t st, ctrl_source_t src, void *user) {
    (void)id;
    (void)st;
    (void)user;
    s_state_dirty = true;

    if (src != SRC_AUTO) {
        static int64_t s_last_event_ms = 0;
        const int64_t  now             = esp_timer_get_time() / 1000;
        // 200ms 合并
        if (now - s_last_event_ms >= 200) {
            s_last_event_ms = now;
            net_link_publish_event("device", home_ctrl_dev_name(id));
        }
    }
}

// 语音转设备动作

// 字库外的字画框
static char     s_voice_cmd_cn[24]; // 识别到的指令
static char     s_voice_act_cn[28]; // 动作结果
static uint32_t s_voice_tick = 0;   // 事件计数

// 防读写撕裂
static portMUX_TYPE s_voice_fb_mux = portMUX_INITIALIZER_UNLOCKED;

// NULL 只显示指令
static void voice_set_feedback(const char *cmd_cn, const char *act_cn) {
    char l1[sizeof(s_voice_cmd_cn)];
    char l2[sizeof(s_voice_act_cn)];

    snprintf(l1, sizeof(l1), "%s", (cmd_cn != NULL) ? cmd_cn : "");
    snprintf(l2, sizeof(l2), "%s", (act_cn != NULL) ? act_cn : "");

    portENTER_CRITICAL(&s_voice_fb_mux);
    memcpy(s_voice_cmd_cn, l1, sizeof(s_voice_cmd_cn));
    memcpy(s_voice_act_cn, l2, sizeof(s_voice_act_cn));
    s_voice_tick++;
    portEXIT_CRITICAL(&s_voice_fb_mux);

    ESP_LOGI(TAG, "屏幕反馈: [%s] %s", l1, l2);
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
        const esp_err_t err = home_command_execute(dev, action, -1, SRC_VOICE, detail);

        char act[28];
        if (err == ESP_OK) {
            if (strcmp(dev, "mode") == 0) {
                snprintf(act, sizeof(act), "已切换到%s模式", (strcmp(action, "auto") == 0) ? "自动" : "手动");
            } else if (strcmp(dev, "buzzer") == 0) {
                snprintf(act, sizeof(act), "蜂鸣器 已%s", (strcmp(action, "off") == 0) ? "静音" : "打开");
            } else {
                snprintf(act, sizeof(act), "%s 已%s", voice_dev_cn(dev),
                         (strcmp(action, "off") == 0 || strcmp(action, "close") == 0) ? "关闭" : "打开");
            }
        } else if (err == ESP_ERR_INVALID_STATE && voice_light_blocked()) {
            snprintf(act, sizeof(act), "未执行: 自动模式");
        } else {
            snprintf(act, sizeof(act), "执行失败");
        }
        voice_set_feedback(bsp_voice_cmd_cn(cmd), act);

        if (err == ESP_ERR_INVALID_STATE && voice_light_blocked()) {
            // 必须回一句
            bsp_voice_speak(AUTO_MODE_HINT_TEXT);
        } else if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            bsp_voice_speak("操作没有成功，请检查设备");
            ESP_LOGW(TAG, "语音指令 %s/%s 执行失败: %s", dev, action,
                     detail[0] ? detail : home_command_err_text(err));
        }
        return;
    }

    sensor_snapshot_t sn;
    bsp_sensors_get(&sn);

    switch (cmd) {
    case VOICE_CMD_QUERY_TEMP:
        bsp_voice_speak_temp(sn.temperature, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_HUMI:
        bsp_voice_speak_humi(sn.humidity, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_ALL:
        bsp_voice_speak_temp_humi(sn.temperature, sn.humidity, sn.th_valid);
        break;
    case VOICE_CMD_QUERY_WEATHER:
        bsp_voice_speak_weather(sn.raining, sn.rain_valid);
        break;
    case VOICE_CMD_QUERY_LIGHT:
        bsp_voice_speak_light(sn.light_pct, sn.light_valid);
        break;
    case VOICE_CMD_QUERY_STATUS:
        // 温湿度加天气
        bsp_voice_speak_temp_humi(sn.temperature, sn.humidity, sn.th_valid);
        bsp_voice_speak_weather(sn.raining, sn.rain_valid);
        break;
    default:
        ESP_LOGD(TAG, "语音指令 %d 无需动作", (int)cmd);
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
static esp_err_t on_injected_voice(const char *token) {
    const voice_cmd_t cmd = bsp_voice_token_to_cmd(token);
    if (cmd == VOICE_CMD_NONE) {
        return ESP_ERR_NOT_FOUND;
    }
    voice_on_cmd(cmd, NULL);
    return ESP_OK;
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
        ESP_LOGI(TAG, "语音助手被唤醒");
        // 逗号用半角
        voice_set_feedback("已唤醒", "在听, 请说指令");
        break;

    case VOICE_EVENT_RAW:
        // 不认识只记日志
        ESP_LOGD(TAG, "语音原始内容: %s", raw != NULL ? raw : "(null)");
        break;

    default:
        break;
    }
}

// 无按键板不编译

#if CONFIG_APP_KEY_ENABLE

// 按键回调要轻量
static void on_key_event(bsp_key_id_t key, bsp_key_event_t event, void *user) {
    (void)user;
    char detail[HOME_CMD_DETAIL_LEN] = {0};

    if (key == BSP_KEY_1) {
        if (event == BSP_KEY_EVENT_SHORT) {
            const char *want = auto_policy_is_auto() ? "manual" : "auto";
            (void)home_command_execute("mode", want, -1, SRC_BUTTON, detail);
            bsp_voice_speak_mode(!auto_policy_is_auto());
            net_link_publish_event("key", "mode");
        } else if (event == BSP_KEY_EVENT_LONG) {
            // 长按重复上报
            if (!s_key1_long_done) {
                s_key1_long_done = true;
                auto_policy_reset_defaults();
                bsp_voice_speak("已恢复默认设置");
                ESP_LOGW(TAG, "KEY1 长按：阈值已恢复默认值");
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
            net_link_publish_event("key", "all_toggle");
        } else if (event == BSP_KEY_EVENT_LONG) {
            // 长按只翻转一次
            if (!s_key2_long_done) {
                s_key2_long_done = true;
                (void)home_command_execute("door", "toggle", -1, SRC_BUTTON, detail);
                net_link_publish_event("key", "door_toggle");
            }
        } else {
            // 松开复位锁
            s_key2_long_done = false;
        }
    }
}

#else

// 不放空实现

// 按键语义收尾
#endif

// 主循环

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

    char     fb1[sizeof(s_voice_cmd_cn)];
    char     fb2[sizeof(s_voice_act_cn)];
    uint32_t fb_tick;

    portENTER_CRITICAL(&s_voice_fb_mux);
    memcpy(fb1, s_voice_cmd_cn, sizeof(fb1));
    memcpy(fb2, s_voice_act_cn, sizeof(fb2));
    fb_tick = s_voice_tick;
    portEXIT_CRITICAL(&s_voice_fb_mux);

    snprintf(in->voice_line1, sizeof(in->voice_line1), "%s", fb1);
    snprintf(in->voice_line2, sizeof(in->voice_line2), "%s", fb2);
    in->voice_tick = fb_tick;
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
    return net_link_wifi_ok() ? BSP_STATUS_NO_WIFI : BSP_STATUS_NO_WIFI;
}

// 主循环任务
static void app_loop_task(void *arg) {
    (void)arg;

    const int64_t publish_period_ms = CONFIG_APP_MQTT_PUBLISH_SENSOR_MS;
    int64_t       sensor_acc_ms     = 0;
    int64_t       led_acc_ms        = 0;
    int64_t       state_acc_ms      = 0;

    for (;;) {
        const int64_t t0 = esp_timer_get_time() / 1000;

        // 自动联动
        sensor_snapshot_t sn;
        bsp_sensors_get(&sn);
        home_auto_tick(&sn);

        // 推屏
        display_view_input_t view;
        build_view_input(&sn, &view);
        display_view_update(&view);

        // 周期上报
        sensor_acc_ms += APP_LOOP_TICK_MS;
        if (sensor_acc_ms >= publish_period_ms) {
            sensor_acc_ms = 0;
            if (link_online()) {
                net_link_publish_sensor(&sn);
                net_link_publish_weather(&sn);
            }
        }

        // 变化立刻上报
        state_acc_ms += APP_LOOP_TICK_MS;
        if (s_state_dirty && (state_acc_ms >= 200 || !link_online())) {
            if (link_online()) {
                net_link_publish_state();
                state_acc_ms = 0;
            }
            s_state_dirty = false;
        }

        // 状态灯
        led_acc_ms += APP_LOOP_TICK_MS;
        if (led_acc_ms >= STATUS_LED_PERIOD_MS) {
            led_acc_ms = 0;
            bsp_status_led_show(status_led_code());
        }

        // 补偿耗时防漂移
        const int64_t elapsed  = (esp_timer_get_time() / 1000) - t0;
        int64_t       sleep_ms = APP_LOOP_TICK_MS - elapsed;
        if (sleep_ms < 10) {
            sleep_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

// 串口调试台

#if CONFIG_APP_SERIAL_DEBUG_ENABLE

#define CONSOLE_LINE_MAX 160

static void console_print_help(void) {
    ESP_LOGI(TAG, "==================== 串口调试台命令 ====================");
    ESP_LOGI(TAG, "  help                      显示本帮助");
    ESP_LOGI(TAG, "  status                    设备 + 传感器 + 网络 + 模式 + 阈值");
    ESP_LOGI(TAG, "  on <dev> / off <dev>      开/关设备");
    ESP_LOGI(TAG, "  toggle <dev>              翻转设备");
    ESP_LOGI(TAG, "  set <dev> <0-100>         灯=亮度，门=开合度(>=50 视为开)");
    ESP_LOGI(TAG, "  all on | all off          4 路灯全亮 / 全灭（绝不动门）");
    ESP_LOGI(TAG, "  open door | close door    开门（会联动亮客厅灯）/ 关门");
    ESP_LOGI(TAG, "  mode auto | mode manual   切换手动/自动模式（存 NVS）");
    ESP_LOGI(TAG, "  buzzer on|off|mute        蜂鸣器开 / 关 / 静音锁");
    ESP_LOGI(TAG, "  cfg                       打印当前阈值");
    ESP_LOGI(TAG, "  cfg <key> <value>         改阈值，例如 cfg buzzer_on_c 26");
    ESP_LOGI(TAG, "  cfg save | cfg reset      保存 / 恢复默认阈值");
    ESP_LOGI(TAG, "  say <cmd>                 ★ 注入一条语音指令，走完整语音链路");
    ESP_LOGI(TAG, "  help-voice                列出全部语音指令短名 + 中文说法 + 拼音命令词表");
    ESP_LOGI(TAG, "  mic                       ★麦克风电平表 5 秒：判断 INMP441 有没有采到声音");
    ESP_LOGI(TAG, "  i2c-scan                  ★实时扫 I2C：查屏/传感器认不到时边插边看（不用复位）");
    ESP_LOGI(TAG, "  i2c-watch [秒]            ★I2C 抖动测试：一边动线一边看设备忽有忽无（默认 20 秒）");
    ESP_LOGI(TAG, "  sensor                    立刻采一次传感器并打印");
    ESP_LOGI(TAG, "  screen                    强制重画一屏（调屏排版用）");
    ESP_LOGI(TAG, "  screen-test               屏幕自检：纯色轮播 + 边框/交叉线（排查不亮/白屏/偏色）");
    ESP_LOGI(TAG, "  screen-blk                显示闪烁 3 次（确认屏受软件控制；OLED 无背光）");
    ESP_LOGI(TAG, "  font-test                 ★字号对照：小字 vs 放大2倍 并排显示，验证固件版本");
    ESP_LOGI(TAG, "  panel-retry               ★重跑屏幕初始化+自检（改完接线不用重新烧录）");
    ESP_LOGI(TAG, "  disp-off / disp-on        ★只发 DISPOFF/DISPON：屏全黑=控制器收到了命令（OLED 无背光）");
    ESP_LOGI(TAG, "  buzzer-test               ★蜂鸣器极性自检：听哪一段安静，据此定极性");
    ESP_LOGI(TAG, "  hw                        硬件诊断：引脚实际电平 + 关键外设状态");
    ESP_LOGI(TAG, "  wifi | mqtt               打印 WiFi / MQTT 连接状态与 topic");
    ESP_LOGI(TAG, "  reboot                    重启");
    ESP_LOGI(TAG, "  <dev> 取值: led_living led_kitchen led_bedroom led_bath door");
    ESP_LOGI(TAG, "======================================================");
}

static void console_print_status(void) {
    ESP_LOGI(TAG, "---------- 设备 ----------");
    home_ctrl_dump();
    ESP_LOGI(TAG, "---------- 传感器 ----------");
    sensor_snapshot_t sn;
    bsp_sensors_get(&sn);
    ESP_LOGI(TAG, "  温度 %.1fC (valid=%d)  湿度 %.1f%% (valid=%d)", sn.temperature, (int)sn.th_valid,
             sn.humidity, (int)sn.th_valid);
    ESP_LOGI(TAG, "  光照 %.1f%% (%.1flux, valid=%d, %s)", sn.light_pct, sn.light_lux, (int)sn.light_valid,
             bsp_sensors_light_is_lux() ? "BH1750" : "光敏电阻");
    ESP_LOGI(TAG, "  雨滴 %.1f%% 天气=%s (valid=%d, 阈值 %.0f%%)", sn.rain_pct,
             sn.raining ? "Raining!" : "Sunny", (int)sn.rain_valid, bsp_sensors_get_rain_threshold());
    ESP_LOGI(TAG, "---------- 网络 ----------");
    net_link_dump();
    ESP_LOGI(TAG, "---------- 自动化 ----------");
    auto_policy_dump();
    ESP_LOGI(TAG, "  蜂鸣器静音锁 = %s", auto_policy_buzzer_muted() ? "生效中" : "未生效");
    ESP_LOGI(TAG, "----------------------------");
}

// 执行一行命令
static void console_run_line(char *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 &&
           (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
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
        ESP_LOGI(TAG, "采样: %.1fC / %.1f%% / 光照 %.1f%% / 雨滴 %.1f%% (%s)", sn.temperature, sn.humidity,
                 sn.light_pct, sn.rain_pct, sn.raining ? "Raining!" : "Sunny");
        return;
    }
    if (strcmp(line, "screen") == 0) {
        display_view_force_redraw();
        ESP_LOGI(TAG, "已请求重画屏幕");
        return;
    }
    // 屏幕自检
    if (strcmp(line, "screen-test") == 0 || strncmp(line, "screen-test ", 12) == 0) {
        int hold = 0;
        if (strlen(line) > 12) {
            hold = atoi(line + 12);
        }
        ESP_LOGI(TAG, "开始屏幕自检（每屏 %d ms，0=默认）...", hold);
        panel_selftest(hold);
        display_view_force_redraw(); // 画回业务界面
        return;
    }
    if (strcmp(line, "screen-blk") == 0) {
        panel_blink_backlight(3, 400, 400);
        return;
    }
    // 字号对照
    if (strcmp(line, "font-test") == 0) {
        ESP_LOGI(TAG, "字号对照测试：第 1/3 行是小字(8x8)，第 2 行是放大 2 倍(=16x16)，第 4 行是大字 ASCII");
        panel_clear(PANEL_BLACK);
        // 小字
        panel_draw_text_cn(0, 0, "温湿 28.7C", PANEL_WHITE, PANEL_TRANSPARENT);
        // 放大 2 倍
        panel_draw_text_cn_scaled(0, 12, "温湿 28.7C", PANEL_WHITE, PANEL_TRANSPARENT, 2);
        // ASCII 大字
        panel_draw_text(0, 40, "28.7C abc", PANEL_WHITE, PANEL_TRANSPARENT);
        panel_flush_all();
        ESP_LOGI(TAG, "★ 判读：若第 1 行明显【小于】第 2 行 → 小字版生效(正常)；"
                      "若两行一样大 → 烧的还是旧固件");
        vTaskDelay(pdMS_TO_TICKS(6000));
        display_view_force_redraw(); // 画回业务界面
        return;
    }
    if (strcmp(line, "disp-off") == 0) {
        panel_display_test(false);
        return;
    }
    if (strcmp(line, "disp-on") == 0) {
        panel_display_test(true);
        return;
    }
    // 重跑屏初始化
    if (strcmp(line, "panel-retry") == 0) {
        ESP_LOGW(TAG, "重跑屏幕初始化（复用总线，只重发初始化序列）...");
        // 必须走 retry
        const esp_err_t perr = display_view_retry_panel();
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "屏幕重新初始化失败: %s", esp_err_to_name(perr));
            return;
        }
        ESP_LOGI(TAG, "屏幕重新初始化成功，接着跑一次自检");
        panel_selftest(0);
        display_view_force_redraw();
        return;
    }
    if (strcmp(line, "buzzer-test") == 0) {
        (void)bsp_buzzer_polarity_test();
        return;
    }
    if (strcmp(line, "buzzer-tick") == 0) {
        // 手动轮询一次
        bsp_buzzer_tick();
        ESP_LOGI(TAG, "buzzer_tick 已执行（短鸣到期检查）");
        return;
    }
    // 麦克风电平表
    if (strcmp(line, "mic") == 0) {
        ESP_LOGI(TAG, "---------- 麦克风电平表（5 秒）----------");
        ESP_LOGI(TAG, "对着 INMP441 说话：rms/peak 应明显变大；全程 0 就是接线或供电问题");
        if (!bsp_voice_ready()) {
            ESP_LOGI(TAG, "语音通道还没初始化（bsp_voice_init 没跑过？）");
            return;
        }
        int peak_max = 0;
        for (int i = 0; i < 25; i++) {
            int        rms  = 0;
            int        peak = 0;
            int        db   = 0;
            const bool has  = bsp_voice_mic_level(&rms, &peak, &db);
            if (peak > peak_max) {
                peak_max = peak;
            }
            ESP_LOGI(TAG, "  [%2d/25] rms=%-6d peak=%-6d dBFS=%d.%d%s", i + 1, rms, peak, db / 10,
                     (db < 0 ? -db : db) % 10, has ? "" : "  (尚未采到数据)");
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (peak_max > 0) {
            ESP_LOGI(TAG, "5 秒内最大峰值=%d → 采到声音了。若仍不识别，查 model 分区/唤醒词/命令词拼音",
                     peak_max);
        } else if (bsp_voice_mic_data_in_other_slot()) {
            // 数据在另一时隙
            ESP_LOGI(TAG, "5 秒内主声道全程 0，但【另一个 I²S 时隙有信号】：");
            ESP_LOGI(TAG, "  → 数据跑到右声道了。把 INMP441 的 L/R 接到 GND 即可");
            ESP_LOGI(TAG, "    （L/R 悬空或接 3V3 都会这样，与供电、SD 无关）。");
        } else {
            ESP_LOGI(TAG, "5 秒内全程 0（两个时隙都没有信号）→ 没采到声音。按顺序查：");
            ESP_LOGI(TAG, "  SD/SCK/WS 是否接反或松了；VDD 是否接了 3V3；L/R 是否接了 GND");
        }
        return;
    }
    // 实时扫 I2C
    if (strcmp(line, "i2c-scan") == 0) {
        uint8_t   found[BSP_I2C_SCAN_MAX];
        const int hits = bsp_i2c_scan(found, sizeof(found));

        ESP_LOGI(TAG, "本工程期望的地址：0x38=AHT20 温湿度  0x3C/0x3D=SSD1306 屏  "
                      "0x23=BH1750 光照  0x44=SHT30 温湿度");
        bool panel_here = false;
        for (int i = 0; i < hits; i++) {
            if (found[i] == BSP_I2C_ADDR_SSD1306 || found[i] == BSP_I2C_ADDR_SSD1306_ALT) {
                panel_here = true;
            }
        }
        if (panel_here) {
            ESP_LOGI(TAG, "★ 屏在总线上（0x3C/0x3D 有应答）→ 接线没问题，"
                          "若屏仍全黑请敲 panel-retry 重跑初始化；还黑就查模块是否 SH1106（需改列偏移）");
        } else {
            ESP_LOGW(TAG, "屏【不在】总线上（0x3C/0x3D 都没应答）—— 按顺序查：");
            ESP_LOGW(TAG, "  ① 屏的 VCC 对 GND 是否有 3.3V（模块有 LDO 才能接 5V，裸屏必须 3.3V）");
            ESP_LOGW(TAG, "  ② SDA→GPIO%d / SCL→GPIO%d 是否松、是否插在了不同的面包板排上",
                     (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
            ESP_LOGW(TAG, "  ③ 快速判别法：把屏的 SDA/SCL 两根线拔下来，插到 AHT20(0x38) 正在用的"
                          "那两排孔上，再敲一次 i2c-scan ——");
            ESP_LOGW(TAG, "     出现了 0x3C → 屏和模块都好，坏的是它原来那两根线/两个孔；");
            ESP_LOGW(TAG, "     仍然没有 → 供电或模块本身的问题（与总线、与固件无关）。");
        }
        return;
    }
    // I2C 抖动测试
    if (strncmp(line, "i2c-watch", 9) == 0) {
        int seconds = 20;
        if (line[9] == ' ') {
            const int v = atoi(&line[10]);
            if (v > 0 && v <= 120) {
                seconds = v;
            }
        }

        static const struct {
            uint8_t     addr;
            const char *name;
        } watch[] = {
            {    BSP_I2C_ADDR_SSD1306,     "SSD1306 屏 (0x3C)"},
            {BSP_I2C_ADDR_SSD1306_ALT,     "SSD1306 屏 (0x3D)"},
            {      BSP_I2C_ADDR_AHT20, "AHT20 温湿度 (0x38)"},
        };
        const int n_watch = (int)(sizeof(watch) / sizeof(watch[0]));

        bool last[3] = {false, false, false};
        int flips[3] = {0, 0, 0};

        // 先探基线
        for (int i = 0; i < n_watch; i++) {
            last[i] = bsp_i2c_probe(watch[i].addr);
        }

        ESP_LOGI(TAG, "---------- I2C 抖动测试：%d 秒，每 300ms 探一次 ----------", seconds);
        ESP_LOGI(TAG, "基线：0x3C=%s  0x3D=%s  0x38=%s", last[0] ? "在线" : "不在线",
                 last[1] ? "在线" : "不在线", last[2] ? "在线" : "不在线");
        ESP_LOGI(TAG, "现在请【轻轻地】依次动每根线（SDA/SCL/VCC/GND，一根一根来），");
        ESP_LOGI(TAG, "本命令只在状态【变化】时打印 —— 哪根线一动就跳，坏的就是它。");

        const int64_t t_end = esp_timer_get_time() / 1000 + (int64_t)seconds * 1000;
        while ((esp_timer_get_time() / 1000) < t_end) {
            for (int i = 0; i < n_watch; i++) {
                const bool now = bsp_i2c_probe(watch[i].addr);
                if (now != last[i]) {
                    flips[i]++;
                    ESP_LOGW(TAG, "  [%4lld ms] %-22s → %s", (long long)(esp_timer_get_time() / 1000),
                             watch[i].name, now ? "出现 ✅" : "消失 ❌");
                    last[i] = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        ESP_LOGI(TAG, "---------- 抖动测试结束 ----------");
        for (int i = 0; i < n_watch; i++) {
            ESP_LOGI(TAG, "  %-22s 跳变 %d 次，结束时%s", watch[i].name, flips[i],
                     last[i] ? "在线" : "不在线");
            if (flips[i] > 0) {
                ESP_LOGW(TAG, "  ↑ 这个地址在动线时忽有忽无 = 接触不良："
                              "换一根杜邦线、换一排孔，或按住模块排针试");
            }
        }
        if (flips[0] + flips[1] + flips[2] == 0) {
            ESP_LOGI(TAG, "全程没有任何跳变：这段时间里接线是稳定的。");
            ESP_LOGI(TAG, "  若屏此刻仍不亮，说明不是接触问题 —— 敲 i2c-scan 看它在不在总线上。");
        }
        return;
    }
    // 硬件诊断
    if (strcmp(line, "hw") == 0) {
        ESP_LOGI(TAG, "---------- 硬件诊断 ----------");
        ESP_LOGI(TAG, "模块自检表：");
        ESP_LOGI(
            TAG,
            "  状态灯=%-3s I2C=%-3s ADC=%-3s 灯=%-3s 舵机=%-3s 蜂鸣器=%-3s 传感器=%-3s 屏=%-3s 按键=%-3s "
            "语音=%-3s",
            board_module_ok(BOARD_MOD_STATUS_LED) ? "OK" : "NO", board_module_ok(BOARD_MOD_I2C) ? "OK" : "NO",
            board_module_ok(BOARD_MOD_ADC) ? "OK" : "NO", board_module_ok(BOARD_MOD_LED) ? "OK" : "NO",
            board_module_ok(BOARD_MOD_SERVO) ? "OK" : "NO", board_module_ok(BOARD_MOD_BUZZER) ? "OK" : "NO",
            board_module_ok(BOARD_MOD_SENSOR) ? "OK" : "NO", board_module_ok(BOARD_MOD_PANEL) ? "OK" : "NO",
            board_module_ok(BOARD_MOD_KEY) ? "OK" : "NO", board_module_ok(BOARD_MOD_VOICE) ? "OK" : "NO");
        ESP_LOGI(TAG, "显示屏   = %s，走 I2C0（无背光引脚）", BSP_PANEL_NAME);
        ESP_LOGI(TAG, "          SDA=GPIO%d  SCL=GPIO%d  地址=0x%02X（备选 0x%02X）", (int)BSP_I2C_SDA_GPIO,
                 (int)BSP_I2C_SCL_GPIO, BSP_I2C_ADDR_SSD1306, BSP_I2C_ADDR_SSD1306_ALT);
        ESP_LOGI(TAG, "          ⚠ OLED 自己发光、【没有 BLK 背光脚】——");
        ESP_LOGI(TAG, "            原来 TFT 时代【BLK 接 3V3 / 改 BSP_PANEL_BLK_CTRL】那一套已经不存在了。");
        ESP_LOGI(TAG, "            屏全黑就两条路：I2C 没通（敲 panel-retry 看日志）或地址跳线不对。");
        ESP_LOGI(TAG, "蜂鸣器   = GPIO%d，配置为【%s】，逻辑电平=%d（0 = 应为静音）", (int)BSP_BUZZER_GPIO,
                 BSP_BUZZER_ACTIVE_LOW ? "低电平触发" : "高电平触发", bsp_gpio_read(BSP_BUZZER_GPIO));
        ESP_LOGI(TAG, "        ⚠ 如果上面电平=0 而蜂鸣器【仍然一直响】：");
        ESP_LOGI(TAG, "          说明你的模块是【低电平触发型】——");
        ESP_LOGI(TAG, "          把 board_config.h 的 BSP_BUZZER_ACTIVE_LOW 改成 1，重新编译烧录。");
#if CONFIG_APP_KEY_ENABLE
        ESP_LOGI(TAG, "按键     KEY1=GPIO%d 电平=%d  KEY2=GPIO%d 电平=%d（0=按下）", (int)BSP_KEY_GPIO_KEY1,
                 bsp_gpio_read(BSP_KEY_GPIO_KEY1), (int)BSP_KEY_GPIO_KEY2, bsp_gpio_read(BSP_KEY_GPIO_KEY2));
#else
        ESP_LOGI(TAG, "按键     未启用（这块板子没有按键，控制走语音 / 手机 / 串口）");
#endif
        // 麦克风电平自检
        int        mic_rms      = 0;
        int        mic_peak     = 0;
        int        mic_db       = 0;
        const bool mic_has_data = bsp_voice_mic_level(&mic_rms, &mic_peak, &mic_db);
        ESP_LOGI(TAG, "语音输入 INMP441 I²S%d SCK=GPIO%d WS=GPIO%d SD=GPIO%d @%dHz", (int)BSP_I2S_MIC_PORT,
                 (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO,
                 BSP_I2S_MIC_SAMPLE_RATE);
        ESP_LOGI(TAG, "         唤醒词「%s」 已唤醒=%s 识别就绪=%s", bsp_voice_wake_word(),
                 bsp_voice_is_awake() ? "是" : "否", bsp_voice_ready() ? "是" : "否");
        ESP_LOGI(TAG, "         麦克风电平 rms=%d peak=%d dBFS=%d.%d（采到数据=%s）", mic_rms, mic_peak,
                 mic_db / 10, (mic_db < 0 ? -mic_db : mic_db) % 10, mic_has_data ? "是" : "否");
        if (!mic_has_data) {
            ESP_LOGI(TAG, "         ⚠ 一直没采到数据：查 INMP441 的 VDD=3V3 / GND / L-R=GND，");
            ESP_LOGI(TAG, "           以及 SCK=GPIO%d WS=GPIO%d SD=GPIO%d 三根线是否接反。",
                     (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
        }
        ESP_LOGI(TAG, "-------------------------------");
        return;
    }
    if (strcmp(line, "wifi") == 0 || strcmp(line, "mqtt") == 0) {
        net_link_dump();
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        ESP_LOGW(TAG, "重启中 ...");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
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
            ESP_LOGI(TAG, "cfg save -> %s", esp_err_to_name(auto_policy_save()));
            return;
        }
        if (strcmp(rest, "reset") == 0) {
            ESP_LOGI(TAG, "cfg reset -> %s", esp_err_to_name(auto_policy_reset_defaults()));
            return;
        }
        // 值有字符串，不走路由
        char *key = rest;
        char *val = strchr(rest, ' ');
        if (val == NULL) {
            ESP_LOGW(TAG, "用法: cfg <key> <value>  或  cfg / cfg save / cfg reset");
            return;
        }
        *val++ = '\0';
        while (*val == ' ') {
            val++;
        }

        const esp_err_t err = auto_policy_set_threshold(key, val);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "cfg %s = %s", key, val);
        } else {
            ESP_LOGW(TAG, "cfg %s %s 被拒: %s", key, val, esp_err_to_name(err));
            ESP_LOGW(TAG, "  提示：改上下限时先发 *_on 再发 *_off；键名见 automation.h");
        }
        return;
    }

    // say 注入
    if (strncmp(line, "say ", 4) == 0) {
        const char *token = line + 4;
        while (*token == ' ') {
            token++;
        }
        const esp_err_t err = bsp_voice_inject(token);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "不认识的语音指令 \"%s\"（输入 help-voice 看全部）", token);
        }
        return;
    }

    if (strncmp(line, "all ", 4) == 0) {
        const char     *act = line + 4;
        const esp_err_t err = home_command_execute("all", act, -1, SRC_CONSOLE, detail);
        ESP_LOGI(TAG, "all %s -> %s %s", act, esp_err_to_name(err), detail);
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
            ESP_LOGW(TAG, "未知命令 \"%s\"（输入 help 看全部）", verb);
            return;
        }

        const esp_err_t err = home_command_execute(dev_final, action_final, pct, SRC_CONSOLE, detail);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s %s %s -> OK", verb, dev_final, action_final);
        } else {
            ESP_LOGW(TAG, "%s %s %s -> %s（%s）", verb, dev_final, action_final, esp_err_to_name(err),
                     detail[0] ? detail : home_command_err_text(err));
        }
        return;
    }

    ESP_LOGW(TAG, "未知命令 \"%s\"（输入 help 看全部）", line);
}

// 串口接收缓冲
#define CONSOLE_UART_RX_BUF 4096

// 必须装驱动
static void console_task(void *arg) {
    (void)arg;

    const esp_err_t derr = uart_driver_install(UART_NUM_0, CONSOLE_UART_RX_BUF, 0, 0, NULL, 0);
    if (derr != ESP_OK && derr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "串口调试台：UART0 驱动安装失败(%s)，调试命令将不可用（其它功能不受影响）",
                 esp_err_to_name(derr));
    }

    static char line[CONSOLE_LINE_MAX];
    size_t      len = 0;

    ESP_LOGI(TAG, "串口调试台已启动 —— 输入 help 查看命令");

    for (;;) {
        uint8_t   ch = 0;
        const int n  = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                console_run_line(line);
                len = 0;
            }
            continue;
        }

        // 退格
        if (ch == 0x08 || ch == 0x7F) {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (len < CONSOLE_LINE_MAX - 1) {
            line[len++] = (char)ch;
        } else {
            len = 0;
            ESP_LOGW(TAG, "命令太长，已丢弃（上限 %d 字节）", CONSOLE_LINE_MAX - 1);
        }
    }
}

// 串口调试台收尾
#endif

// NVS 要擦一次重建
static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除重建 (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// 应用入口
void app_main(void) {
    s_boot_ms = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "###########################################################");
    ESP_LOGI(TAG, "#  ESP32-S3 智能家居（光影语音版）");
    ESP_LOGI(TAG, "#  ESP-IDF %s   build %s %s", esp_get_idf_version(), __DATE__, __TIME__);
    ESP_LOGI(TAG, "###########################################################");

    const esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s（阈值将无法保存）", esp_err_to_name(nvs_err));
    }

    // 缺配件只告警
    board_dump_pinmap();
    board_init();

    // 有依赖顺序
    ESP_ERROR_CHECK(home_ctrl_init());
    ESP_ERROR_CHECK(home_auto_init());

    (void)home_ctrl_register_cb(on_device_changed, NULL);

    // 自检前别起任务
#if CONFIG_APP_PANEL_BOOT_SELFTEST
    if (display_view_init_no_task() == ESP_OK) {
        ESP_LOGI(TAG, "开机屏幕自检：接下来屏幕会依次显示 全亮/全灭/左右/上下/棋盘/自检卡");
        panel_selftest(0);
        (void)display_view_start();

        sensor_snapshot_t sn0;
        bsp_sensors_get(&sn0);
        display_view_input_t v0;
        build_view_input(&sn0, &v0);
        display_view_update(&v0);
        display_view_force_redraw();
    } else {
        ESP_LOGW(TAG, "屏幕不可用 —— 其余功能照常（配件没到也能跑）");
    }
#else
    if (display_view_init() != ESP_OK) {
        ESP_LOGW(TAG, "屏幕不可用 —— 其余功能照常（配件没到也能跑）");
    } else {
        // 先推一帧
        sensor_snapshot_t sn0;
        bsp_sensors_get(&sn0);
        display_view_input_t v0;
        build_view_input(&sn0, &v0);
        display_view_update(&v0);
        display_view_force_redraw();
    }
#endif

    // 无按键不注册
#if CONFIG_APP_KEY_ENABLE
    bsp_key_register_cb(BSP_KEY_1, on_key_event, NULL);
    bsp_key_register_cb(BSP_KEY_2, on_key_event, NULL);
    ESP_LOGI(TAG, "按键已启用：KEY1(短按切模式/长按恢复阈值) KEY2(短按全灯/长按开关门)");
#else
    ESP_LOGI(TAG, "无按键板：控制入口 = 语音 + 手机 MQTT + 串口调试台（CONFIG_APP_KEY_ENABLE=n）");
#endif

    // 语音走同一入口
    ESP_ERROR_CHECK(bsp_voice_register_cb(on_voice_event, NULL));

    home_command_set_voice_handler(on_injected_voice);

    // 传感器自采样
    ESP_ERROR_CHECK(bsp_sensors_start_auto());

    // 联网不阻塞
    ESP_ERROR_CHECK(net_link_init());

    // 蓝牙晚一步起
    // 等 UID 算好
    // 起不来也只告警
#if CONFIG_APP_BLE_ENABLE
    {
        const esp_err_t ble_err = ble_app_start();
        if (ble_err == ESP_OK) {
            ESP_LOGI(TAG, "蓝牙控制通道已启动（手机 App 直连，和 MQTT / 网页并行）");
        } else {
            ESP_LOGW(TAG, "蓝牙没起来：%s —— 手机直连不可用，WiFi/MQTT/网页面板照常运行",
                     esp_err_to_name(ble_err));
        }
    }
#endif

    // 常驻任务
    xTaskCreate(app_loop_task, "app_loop", 4096, NULL, 5, NULL);

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
#endif

    // 状态灯开机中
    bsp_status_led_show(BSP_STATUS_BOOTING);

    ESP_LOGI(TAG, "system ready. free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "（app_main 返回，后续由各任务驱动）");
}
