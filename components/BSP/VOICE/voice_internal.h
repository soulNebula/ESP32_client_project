// c
// 语音内部接口
#pragma once
#include "voice.h"

#ifdef __cplusplus
extern "C" {
#endif

// 回调里别阻塞
void bsp_voice_dispatch(voice_cmd_t cmd);

// 唤醒词命中上报
void bsp_voice_notify_wake(void);

#ifdef __cplusplus
}
#endif
