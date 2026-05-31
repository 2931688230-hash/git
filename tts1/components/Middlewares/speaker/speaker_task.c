#include "speaker_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "esp_log.h"
#include "speaker_player.h"
#include "speaker_test_pcm.h"

static const char *TAG = "speaker_task";

/*
 * audio_task 的用途：
 * 1. 把音频播放从 main task 中移出来，避免 main 长时间写 PCM/I2S 导致 task_wdt。
 * 2. 让独立任务调用 audio_player，由 audio_player 内部通过 ring buffer 驱动 I2S/PDM 输出。
 * 3. main 只负责启动任务和让出 CPU，不能直接做 PCM write。
 */
static void audio_task_entry(void *arg)
{
    (void)arg;

    esp_err_t err = audio_player_play_pcm(pcm_data, pcm_data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker_test_pcm playback failed: %s", esp_err_to_name(err));
    } else {
#if APP_DEBUG_SPEAKER_TEST_PCM_LOG
        ESP_LOGI(TAG, "speaker_test_pcm playback done");
#endif
    }

    vTaskDelete(NULL);
}

esp_err_t audio_task_start(void)
{
    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 创建独立音频任务，播放逻辑在该任务中执行，不占用 main task。 */
    BaseType_t task_created = xTaskCreate(audio_task_entry,
                                          "audio_task",
                                          MAIN_AUDIO_TASK_STACK_SIZE,
                                          NULL,
                                          MAIN_AUDIO_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
