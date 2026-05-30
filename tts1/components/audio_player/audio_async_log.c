#include "audio_async_log.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef AUDIO_ASYNC_LOG_QUEUE_LENGTH
#define AUDIO_ASYNC_LOG_QUEUE_LENGTH 64
#endif

#ifndef AUDIO_ASYNC_LOG_TASK_STACK_SIZE
#define AUDIO_ASYNC_LOG_TASK_STACK_SIZE 4096
#endif

#ifndef AUDIO_ASYNC_LOG_TASK_PRIORITY
#define AUDIO_ASYNC_LOG_TASK_PRIORITY 1
#endif

typedef enum {
    AUDIO_ASYNC_LOG_EVENT_TEXT = 0,
    AUDIO_ASYNC_LOG_EVENT_ERR,
    AUDIO_ASYNC_LOG_EVENT_VALUES,
} audio_async_log_event_type_t;

typedef struct {
    audio_async_log_event_type_t type;
    audio_async_log_level_t level;
    const char *tag;
    const char *message;
    esp_err_t err;
    int64_t values[4];
} audio_async_log_event_t;

static QueueHandle_t s_audio_log_queue = NULL;
static TaskHandle_t s_audio_log_task = NULL;

static void audio_async_log_emit(const audio_async_log_event_t *event)
{
    const char *tag = event->tag != NULL ? event->tag : "audio";
    const char *message = event->message != NULL ? event->message : "";

    switch (event->type) {
    case AUDIO_ASYNC_LOG_EVENT_ERR:
        switch (event->level) {
        case AUDIO_ASYNC_LOG_ERROR:
            ESP_LOGE(tag, "%s: err=0x%x(%s)",
                     message,
                     (unsigned int)event->err,
                     esp_err_to_name(event->err));
            break;
        case AUDIO_ASYNC_LOG_WARN:
            ESP_LOGW(tag, "%s: err=0x%x(%s)",
                     message,
                     (unsigned int)event->err,
                     esp_err_to_name(event->err));
            break;
        case AUDIO_ASYNC_LOG_DEBUG:
            ESP_LOGD(tag, "%s: err=0x%x(%s)",
                     message,
                     (unsigned int)event->err,
                     esp_err_to_name(event->err));
            break;
        default:
            ESP_LOGI(tag, "%s: err=0x%x(%s)",
                     message,
                     (unsigned int)event->err,
                     esp_err_to_name(event->err));
            break;
        }
        break;
    case AUDIO_ASYNC_LOG_EVENT_VALUES:
        switch (event->level) {
        case AUDIO_ASYNC_LOG_ERROR:
            ESP_LOGE(tag, "%s: v0=%lld v1=%lld v2=%lld v3=%lld",
                     message,
                     (long long)event->values[0],
                     (long long)event->values[1],
                     (long long)event->values[2],
                     (long long)event->values[3]);
            break;
        case AUDIO_ASYNC_LOG_WARN:
            ESP_LOGW(tag, "%s: v0=%lld v1=%lld v2=%lld v3=%lld",
                     message,
                     (long long)event->values[0],
                     (long long)event->values[1],
                     (long long)event->values[2],
                     (long long)event->values[3]);
            break;
        case AUDIO_ASYNC_LOG_DEBUG:
            ESP_LOGD(tag, "%s: v0=%lld v1=%lld v2=%lld v3=%lld",
                     message,
                     (long long)event->values[0],
                     (long long)event->values[1],
                     (long long)event->values[2],
                     (long long)event->values[3]);
            break;
        default:
            ESP_LOGI(tag, "%s: v0=%lld v1=%lld v2=%lld v3=%lld",
                     message,
                     (long long)event->values[0],
                     (long long)event->values[1],
                     (long long)event->values[2],
                     (long long)event->values[3]);
            break;
        }
        break;
    case AUDIO_ASYNC_LOG_EVENT_TEXT:
    default:
        switch (event->level) {
        case AUDIO_ASYNC_LOG_ERROR:
            ESP_LOGE(tag, "%s", message);
            break;
        case AUDIO_ASYNC_LOG_WARN:
            ESP_LOGW(tag, "%s", message);
            break;
        case AUDIO_ASYNC_LOG_DEBUG:
            ESP_LOGD(tag, "%s", message);
            break;
        default:
            ESP_LOGI(tag, "%s", message);
            break;
        }
        break;
    }
}

static void audio_async_log_task(void *arg)
{
    (void)arg;

    while (true) {
        audio_async_log_event_t event = {0};
        if (xQueueReceive(s_audio_log_queue, &event, portMAX_DELAY) == pdTRUE) {
            audio_async_log_emit(&event);
            vTaskDelay(1);
        }
    }
}

esp_err_t audio_async_log_init(void)
{
    if (s_audio_log_queue == NULL) {
        s_audio_log_queue = xQueueCreate(AUDIO_ASYNC_LOG_QUEUE_LENGTH,
                                         sizeof(audio_async_log_event_t));
        if (s_audio_log_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_audio_log_task == NULL) {
        BaseType_t created = xTaskCreate(audio_async_log_task,
                                         "audio_log",
                                         AUDIO_ASYNC_LOG_TASK_STACK_SIZE,
                                         NULL,
                                         AUDIO_ASYNC_LOG_TASK_PRIORITY,
                                         &s_audio_log_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void audio_async_log_send(audio_async_log_event_t *event)
{
    if (s_audio_log_queue == NULL) {
        return;
    }

    (void)xQueueSend(s_audio_log_queue, event, 0);
}

void audio_async_log_text(audio_async_log_level_t level,
                          const char *tag,
                          const char *message)
{
    audio_async_log_event_t event = {
        .type = AUDIO_ASYNC_LOG_EVENT_TEXT,
        .level = level,
        .tag = tag,
        .message = message,
    };
    audio_async_log_send(&event);
}

void audio_async_log_err(audio_async_log_level_t level,
                         const char *tag,
                         const char *message,
                         esp_err_t err)
{
    audio_async_log_event_t event = {
        .type = AUDIO_ASYNC_LOG_EVENT_ERR,
        .level = level,
        .tag = tag,
        .message = message,
        .err = err,
    };
    audio_async_log_send(&event);
}

void audio_async_log_values(audio_async_log_level_t level,
                            const char *tag,
                            const char *message,
                            int64_t value0,
                            int64_t value1,
                            int64_t value2,
                            int64_t value3)
{
    audio_async_log_event_t event = {
        .type = AUDIO_ASYNC_LOG_EVENT_VALUES,
        .level = level,
        .tag = tag,
        .message = message,
        .values = {value0, value1, value2, value3},
    };
    audio_async_log_send(&event);
}
