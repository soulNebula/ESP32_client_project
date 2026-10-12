// c
// 指令路由接口
#pragma once
#include <stdbool.h>
#include "app_common.h"
#include "device_model.h"

// 出错原因缓冲长度
#define HOME_CMD_DETAIL_LEN 96

// 不用就传 -1
app_err_t home_command_execute(const char *dev, const char *action, int pct, ctrl_source_t src, char *detail);

typedef app_err_t (*home_voice_inject_fn)(const char *token);

void home_command_set_voice_handler(home_voice_inject_fn cb);

// 错误码换中文原因
const char *home_command_err_text(app_err_t err);
