#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2s.h"
#include "tts.h"
#include "wifi_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    char connected_ssid[33] = {0};

    ESP_LOGI(TAG, "System start");

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());

    // 连接已保存列表中当前信号最强的 WiFi。
    ESP_LOGI(TAG, "WiFi connect start");
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected, SSID: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }

    ESP_LOGI(TAG, "TTS playback start");
    tts_play_text("你好，ESP32-C5");
    ESP_LOGI(TAG, "TTS playback end");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
