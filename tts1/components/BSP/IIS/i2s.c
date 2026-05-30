#include "i2s.h"

#include "audio_player.h"
#include "esp_log.h"

static const char *TAG = "i2s_compat";

void i2s_init(void)
{
    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_init failed: %s", esp_err_to_name(err));
    }
}

void i2s_play(const int16_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if ((len % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "reject unaligned PCM byte length: len=%lu",
                 (unsigned long)len);
        return;
    }

    size_t sample_count = (size_t)len / sizeof(int16_t);
    if (sample_count > UINT32_MAX) {
        ESP_LOGE(TAG, "reject oversized PCM buffer: samples=%zu", sample_count);
        return;
    }

    esp_err_t err = audio_player_play_tts_pcm(data,
                                              (uint32_t)sample_count,
                                              BSP_I2S_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_play_tts_pcm failed: %s", esp_err_to_name(err));
    }
}

void i2s_play_pcm16_mono(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return;
    }
    if (sample_count > UINT32_MAX) {
        ESP_LOGE(TAG, "reject oversized PCM buffer: samples=%zu", sample_count);
        return;
    }

    esp_err_t err = audio_player_play_tts_pcm(samples,
                                              (uint32_t)sample_count,
                                              BSP_I2S_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_play_tts_pcm failed: %s", esp_err_to_name(err));
    }
}
