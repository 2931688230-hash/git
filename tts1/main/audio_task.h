#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动独立 audio_task。
 * 用途：让 main task 不直接输出 PCM，把音频播放交给专用任务处理，
 * 以降低 task_wdt reset、audio drop、crackle 和断续风险。
 */
esp_err_t audio_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_TASK_H */
