#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doubao_tts.h"
#include "i2s.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static void main_tts_pcm_output(const int16_t *samples,
                                size_t sample_count,
                                int sample_rate_hz,
                                void *user_ctx)
{
    (void)user_ctx;

    if (sample_rate_hz != 16000) {
        ESP_LOGW(TAG, "Unexpected sample rate: %d Hz", sample_rate_hz);
    }

    i2s_play_pcm16_mono(samples, sample_count);
}

void app_main(void)
{
    char connected_ssid[33] = {0};

    ESP_LOGI(TAG, "System start");

    ESP_ERROR_CHECK(wifi_manager_init());

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
    ESP_ERROR_CHECK(doubao_tts_init(main_tts_pcm_output, NULL));
    esp_err_t tts_err = doubao_tts_play_text("\xE4""\xBD""\xA0""\xE5""\xA5""\xBD""\xEF""\xBC""\x8C""ESP32-C5");
    if (tts_err != ESP_OK) {
        ESP_LOGE(TAG, "TTS playback failed: %s", esp_err_to_name(tts_err));
    }
    ESP_LOGI(TAG, "TTS playback end");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
