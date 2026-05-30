#ifndef AUDIO_ASYNC_LOG_H
#define AUDIO_ASYNC_LOG_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_ASYNC_LOG_ERROR = 0,
    AUDIO_ASYNC_LOG_WARN,
    AUDIO_ASYNC_LOG_INFO,
    AUDIO_ASYNC_LOG_DEBUG,
} audio_async_log_level_t;

esp_err_t audio_async_log_init(void);

void audio_async_log_text(audio_async_log_level_t level,
                          const char *tag,
                          const char *message);

void audio_async_log_err(audio_async_log_level_t level,
                         const char *tag,
                         const char *message,
                         esp_err_t err);

void audio_async_log_values(audio_async_log_level_t level,
                            const char *tag,
                            const char *message,
                            int64_t value0,
                            int64_t value1,
                            int64_t value2,
                            int64_t value3);

#endif /* AUDIO_ASYNC_LOG_H */
