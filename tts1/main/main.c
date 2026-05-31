#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_main_config.h"
#include "speaker_task.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "System start");

    if (audio_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "audio_task_start failed");
    }

    /* main task 保持空闲让出 CPU，实际 PCM/I2S 输出由 audio_task 处理。 */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}
