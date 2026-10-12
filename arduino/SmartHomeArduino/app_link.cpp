// c
// 四个入口的指令路由

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "device_model.h"
#include "app_cmd.h"

#include "automation.h"

static const char *TAG = "CMD";

// main 里同步
#define AUTO_LIGHT_BLOCKED_MSG "自动模式下灯光由光照控制，请先切到手动模式"

// 失败不改软件态
#define HW_FAIL_MSG "设备无响应（接线或供电问题）"

// 只 4 路灯，不含门
#define DEV_ALL "all"
// 手动/自动
#define DEV_MODE "mode"
#define DEV_VOICE "voice"
// 阈值配置读写
#define DEV_CONFIG "config"
// 打印设备状态
#define DEV_STATUS "status"
// 同 status
#define DEV_GET "get"

// 注入回调
static home_voice_inject_fn s_voice_cb;

// 调用方给够缓冲
static void detail_set(char *detail, const char *text) {
    if (detail == NULL) {
        return;
    }
    snprintf(detail, HOME_CMD_DETAIL_LEN, "%s", (text != NULL) ? text : "");
}

// 超长截断不报错
static void detail_setf(char *detail, const char *fmt, ...) {
    if (detail == NULL || fmt == NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, HOME_CMD_DETAIL_LEN, fmt, ap);
    va_end(ap);
}

static void copy_bounded(char *dst, size_t len, const char *src) {
    if (dst == NULL || len == 0) {
        return;
    }
    snprintf(dst, len, "%s", (src != NULL) ? src : "");
}

// 错误码转文案
static app_err_t translate_ctrl_err(app_err_t err, bool is_light, char *detail) {
    switch (err) {
    case APP_OK:
        detail_set(detail, "ok");
        break;
    case APP_ERR_INVALID_STATE:
        detail_set(detail, is_light ? AUTO_LIGHT_BLOCKED_MSG : home_command_err_text(err));
        break;
    case APP_ERR_FAIL:
        detail_set(detail, HW_FAIL_MSG);
        break;
    default:
        detail_set(detail, home_command_err_text(err));
        break;
    }
    return err;
}

// 单灯动作分发
static app_err_t exec_light(device_id_t id, const char *action, int pct, ctrl_source_t src, char *detail) {
    app_err_t err;

    if (strcmp(action, "on") == 0 || strcmp(action, "open") == 0) {
        err = home_ctrl_set_power(id, true, src);
    } else if (strcmp(action, "off") == 0 || strcmp(action, "close") == 0) {
        err = home_ctrl_set_power(id, false, src);
    } else if (strcmp(action, "toggle") == 0) {
        err = home_ctrl_toggle(id, src);
    } else if (strcmp(action, "level") == 0 || strcmp(action, "set") == 0) {
        if (pct < 0) {
            // 缺省 -1 不猜
            detail_set(detail, "level 动作缺少亮度值（pct 0~100）");
            return APP_ERR_INVALID_ARG;
        }
        err = home_ctrl_set_level(id, pct, src);
    } else {
        char name[32];
        copy_bounded(name, sizeof(name), action);
        detail_setf(detail, "未知动作: %s", name);
        return APP_ERR_NOT_FOUND;
    }

    return translate_ctrl_err(err, true, detail);
}

// 批量结果统一收尾
static app_err_t all_lights_result(int ok_cnt, ctrl_source_t src, char *detail) {
    if (ok_cnt == (int)(DEV_LED_BATH - DEV_LED_LIVING + 1)) {
        detail_set(detail, "ok");
        return APP_OK;
    }

    if (ok_cnt == 0) {
        if (src != SRC_AUTO && auto_policy_is_auto()) {
            detail_set(detail, AUTO_LIGHT_BLOCKED_MSG);
            return APP_ERR_INVALID_STATE;
        }
        detail_set(detail, HW_FAIL_MSG);
        return APP_ERR_FAIL;
    }

    // 部分成功少见
    detail_setf(detail, "部分灯未响应: %d/4 盏生效", ok_cnt);
    return APP_ERR_FAIL;
}

// 只覆盖 4 灯
static app_err_t exec_all(const char *action, int pct, ctrl_source_t src, char *detail) {
    bool on;

    if (strcmp(action, "on") == 0) {
        return all_lights_result(home_ctrl_set_all_lights(true, src), src, detail);
    }
    if (strcmp(action, "off") == 0) {
        return all_lights_result(home_ctrl_set_all_lights(false, src), src, detail);
    }

    if (strcmp(action, "toggle") == 0) {
        // 任一亮则全灭
        on = true;
        for (int i = DEV_LED_LIVING; i <= DEV_LED_BATH; i++) {
            if (home_ctrl_is_on((device_id_t)i)) {
                on = false;
                break;
            }
        }
        return all_lights_result(home_ctrl_set_all_lights(on, src), src, detail);
    }

    if (strcmp(action, "level") == 0 || strcmp(action, "set") == 0) {
        if (pct < 0) {
            detail_set(detail, "level 动作缺少亮度值（pct 0~100）");
            return APP_ERR_INVALID_ARG;
        }
        int ok_cnt = 0;
        for (int i = DEV_LED_LIVING; i <= DEV_LED_BATH; i++) {
            if (home_ctrl_set_level((device_id_t)i, pct, src) == APP_OK) {
                ok_cnt++;
            }
        }
        return all_lights_result(ok_cnt, src, detail);
    }

    {
        char name[32];
        copy_bounded(name, sizeof(name), action);
        detail_setf(detail, "未知动作: %s", name);
    }
    return APP_ERR_NOT_FOUND;
}

// 门：关时不动客厅灯
static app_err_t exec_door(const char *action, int pct, ctrl_source_t src, char *detail) {
    app_err_t err;

    if (strcmp(action, "open") == 0 || strcmp(action, "on") == 0) {
        err = home_ctrl_set_power(DEV_DOOR, true, src);
    } else if (strcmp(action, "close") == 0 || strcmp(action, "off") == 0) {
        err = home_ctrl_set_power(DEV_DOOR, false, src);
    } else if (strcmp(action, "toggle") == 0) {
        err = home_ctrl_toggle(DEV_DOOR, src);
    } else if (strcmp(action, "set") == 0 || strcmp(action, "level") == 0) {
        if (pct < 0) {
            detail_set(detail, "set 动作缺少开合度（pct 0~100）");
            return APP_ERR_INVALID_ARG;
        }

        err = home_ctrl_set_level(DEV_DOOR, pct, src);
        if (err == APP_OK) {
            // 50% 才算开
            int clamped = (pct < 0) ? 0 : ((pct > 100) ? 100 : pct);
            detail_setf(detail, "门开合度 %d%%（%s）", clamped,
                        (clamped >= 50) ? "已判定为开" : "低于 50%，BSP 判定为关");
            return APP_OK;
        }
    } else {
        char name[32];
        copy_bounded(name, sizeof(name), action);
        detail_setf(detail, "未知动作: %s", name);
        return APP_ERR_NOT_FOUND;
    }

    return translate_ctrl_err(err, false, detail);
}

// 蜂鸣器：顺序不能反
static app_err_t exec_buzzer(const char *action, ctrl_source_t src, char *detail) {
    app_err_t err;

    if (strcmp(action, "on") == 0) {
        // 先解锁
        auto_policy_unmute_buzzer();
        err = home_ctrl_set_power(DEV_BUZZER, true, src);
    } else if (strcmp(action, "off") == 0 || strcmp(action, "mute") == 0) {
        // 先上锁
        auto_policy_mute_buzzer();
        err = home_ctrl_set_power(DEV_BUZZER, false, src);
    } else {
        char name[32];
        copy_bounded(name, sizeof(name), action);
        detail_setf(detail, "未知动作: %s", name);
        return APP_ERR_NOT_FOUND;
    }

    return translate_ctrl_err(err, false, detail);
}

// 切换手动/自动
static app_err_t exec_mode(const char *action, char *detail) {
    bool want_auto;

    if (strcmp(action, "auto") == 0 || strcmp(action, "on") == 0 || strcmp(action, "true") == 0 ||
        strcmp(action, "1") == 0) {
        want_auto = true;
    } else if (strcmp(action, "manual") == 0 || strcmp(action, "off") == 0 || strcmp(action, "false") == 0 ||
               strcmp(action, "0") == 0) {
        want_auto = false;
    } else {
        char name[32];
        copy_bounded(name, sizeof(name), action);
        detail_setf(detail, "未知动作: %s", name);
        return APP_ERR_NOT_FOUND;
    }

    const auto_mode_t mode = want_auto ? AUTO_MODE_AUTO : AUTO_MODE_MANUAL;
    const app_err_t   err  = auto_policy_set_mode(mode);
    if (err != APP_OK) {
        detail_set(detail, home_command_err_text(err));
        return err;
    }

    detail_setf(detail, "已切换到%s", auto_policy_mode_name_cn(mode));
    return APP_OK;
}

// 注入语音指令
static app_err_t exec_voice(const char *action, char *detail) {
    if (s_voice_cb == NULL) {
        detail_set(detail, "语音通道未注册回调");
        return APP_ERR_INVALID_STATE;
    }

    const app_err_t err = s_voice_cb(action);
    if (err == APP_OK) {
        detail_set(detail, "ok");
    } else {
        detail_set(detail, home_command_err_text(err));
    }
    return err;
}

// 阈值键值读写
// on 必须先发
static app_err_t exec_config(const char *action, int pct, char *detail) {
    if (action == NULL || action[0] == '\0') {
        // 空值即查询
        auto_policy_dump();

        const auto_cfg_t *cfg = auto_policy_cfg();
        if (cfg == NULL) {
            detail_set(detail, "配置不可用");
            return APP_ERR_INVALID_STATE;
        }

        // 别塞完整 JSON
        detail_setf(detail, "%s 光照 <%d 开 >%d 关, 蜂鸣 %d/%d℃, 级联%s", auto_policy_mode_name_cn(cfg->mode),
                    (int)(cfg->light_on_pct + 0.5f), (int)(cfg->light_off_pct + 0.5f),
                    (int)(cfg->buzzer_on_c + 0.5f), (int)(cfg->buzzer_off_c + 0.5f),
                    cfg->door_light_cascade ? "开" : "关");
        return APP_OK;
    }

    char val[16];
    if (pct >= 0) {
        snprintf(val, sizeof(val), "%d", pct);
    } else {
        // 布尔键传 "1"
        val[0] = '1';
        val[1] = '\0';
    }

    {
        char key[32];
        copy_bounded(key, sizeof(key), action);

        const app_err_t err = auto_policy_set_threshold(key, val);
        if (err == APP_OK) {
            detail_setf(detail, "已更新 %s=%s", key, val);
            return APP_OK;
        }
        if (err == APP_ERR_INVALID_ARG) {
            detail_setf(detail, "未知配置项: %s", key);
            return err;
        }
        if (err == APP_ERR_INVALID_STATE) {
            detail_setf(detail, "%s=%s 不合法（改上下限请先发 on 再发 off）", key, val);
            return err;
        }
        detail_set(detail, home_command_err_text(err));
        return err;
    }
}

// 唯一收敛入口
// 判定顺序不可换
app_err_t home_command_execute(const char *dev, const char *action, int pct, ctrl_source_t src, char *detail) {
    if (detail != NULL) {
        detail[0] = '\0';
    }

    if (dev == NULL || dev[0] == '\0') {
        detail_set(detail, "dev/action 不能为空");
        return APP_ERR_INVALID_ARG;
    }

    LOGD(TAG, "cmd: dev=%s action=%s pct=%d src=%s", dev, (action != NULL) ? action : "(null)", pct,
         home_ctrl_src_name(src));

    if (strcmp(dev, DEV_CONFIG) == 0) {
        return exec_config(action, pct, detail);
    }

    // 只打印
    if (strcmp(dev, DEV_STATUS) == 0 || strcmp(dev, DEV_GET) == 0) {
        home_ctrl_dump();
        detail_set(detail, "ok");
        return APP_OK;
    }

    if (action == NULL || action[0] == '\0') {
        detail_set(detail, "dev/action 不能为空");
        return APP_ERR_INVALID_ARG;
    }

    // 特殊域
    if (strcmp(dev, DEV_ALL) == 0) {
        return exec_all(action, pct, src, detail);
    }
    if (strcmp(dev, DEV_MODE) == 0) {
        return exec_mode(action, detail);
    }
    if (strcmp(dev, DEV_VOICE) == 0) {
        return exec_voice(action, detail);
    }

    // 单个设备
    {
        const device_id_t id = home_ctrl_dev_from_name(dev);
        if (id >= DEV_MAX) {
            char name[32];
            copy_bounded(name, sizeof(name), dev);
            detail_setf(detail, "未知设备: %s", name);
            return APP_ERR_NOT_FOUND;
        }

        if (id == DEV_DOOR) {
            return exec_door(action, pct, src, detail);
        }
        if (id == DEV_BUZZER) {
            return exec_buzzer(action, src, detail);
        }
        return exec_light(id, action, pct, src, detail);
    }
}

// NULL 即注销
void home_command_set_voice_handler(home_voice_inject_fn cb) {
    s_voice_cb = cb;
    LOGD(TAG, "voice handler %s", (cb != NULL) ? "已注册" : "已注销");
}

// 错误码转中文
const char *home_command_err_text(app_err_t err) {
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
    default:
        return "未知错误";
    }
}
