// c
// 时间、日志与错误码
#include "app_common.h"

#include "esp_timer.h"

// 默认只打 INFO 以上
static bool s_verbose = false;

// 每秒时间戳
uint32_t app_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 开机以来的毫秒
int64_t app_uptime_ms(void) {
    return esp_timer_get_time() / 1000;
}

// 开就打印 LOGD
bool app_log_verbose(void) {
    return s_verbose;
}

void app_log_set_verbose(bool on) {
    s_verbose = on;
    LOGI("COMMON", "详细日志 %s", on ? "已打开" : "已关闭");
}

// 错误码转中文
const char *app_err_text(app_err_t err) {
    switch (err) {
    case APP_OK:
        return "成功";
    case APP_ERR_INVALID_ARG:
        return "参数不合法";
    case APP_ERR_NOT_FOUND:
        return "不支持的命令";
    case APP_ERR_INVALID_STATE:
        return "当前状态不允许";
    case APP_ERR_FAIL:
        return "执行失败";
    case APP_ERR_NO_MEM:
        return "内存不足";
    case APP_ERR_NOT_SUPPORTED:
        return "本版本不支持";
    default:
        return "未知错误";
    }
}
