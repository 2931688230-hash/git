#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doubao_tts.h"
#include "i2s.h"
#include "main_config.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/**
 * @brief Doubao-TTS 解码后的 PCM 播放回调。
 *
 * 调用方法：
 * 1. app_main() 在 Wi-Fi 连接成功后调用 doubao_tts_init(main_tts_pcm_output, NULL) 注册本函数；
 * 2. doubao_tts 内部收到 MP3 流并用 minimp3 解码出 PCM 后，会自动回调本函数；
 * 3. 本函数只负责把 PCM16 单声道样本交给现有 I2S Speaker 播放接口。
 *
 * @param samples PCM16 单声道样本数组，只在本次回调期间有效。
 * @param sample_count 样本数量，不是字节数。
 * @param sample_rate_hz 当前音频采样率，应该与 BSP_I2S_SAMPLE_RATE_HZ 保持一致。
 * @param user_ctx doubao_tts_init() 传入的用户上下文；当前没有使用。
 */
static void main_tts_pcm_output(const int16_t *samples,
                                size_t sample_count,
                                int sample_rate_hz,
                                void *user_ctx)
{
    (void)user_ctx;

    if (sample_rate_hz != BSP_I2S_SAMPLE_RATE_HZ) {
        ESP_LOGW(TAG,
                 "TTS sample rate is %d Hz, but I2S is configured as %d Hz",
                 sample_rate_hz,
                 BSP_I2S_SAMPLE_RATE_HZ);
    }

    i2s_play_pcm16_mono(samples, sample_count);
}

/**
 * @brief 播放一段 Doubao-TTS 冒烟测试语音。
 *
 * 调用方法：
 * 1. app_main() 已经完成 Wi-Fi 初始化；
 * 2. wifi_connect_to_ap() 已经返回 ESP_OK，确认设备能访问网络；
 * 3. doubao_tts_init() 已经注册 PCM 输出回调；
 * 4. 调用本函数后会阻塞等待 doubao_tts_play_text() 播放完成。
 *
 * 说明：
 * - 测试文本由 MAIN_TTS_TEST_TEXT 配置；
 * - 播放前等待时间由 MAIN_TTS_TEST_DELAY_MS 配置；
 * - 本函数只用于验证语音链路，后续正式业务可以替换成按键、串口命令或状态机触发。
 */
static void run_tts_smoke_test(void)
{
    vTaskDelay(pdMS_TO_TICKS(MAIN_TTS_TEST_DELAY_MS));

    ESP_LOGI(TAG, "Doubao TTS playback start");
    esp_err_t err = doubao_tts_play_text(MAIN_TTS_TEST_TEXT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Doubao TTS playback failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Doubao TTS playback end");
}

/**
 * @brief ESP-IDF 应用入口函数。
 *
 * 调用方法：
 * - ESP-IDF 启动 FreeRTOS 后会自动调用 app_main()，用户代码不需要手动调用。
 *
 * 当前流程：
 * 1. 初始化 Wi-Fi 管理模块；
 * 2. 连接 wifi_credentials.h 中配置的目标 Wi-Fi；
 * 3. 注册 Doubao-TTS 的 PCM 输出回调；
 * 4. 连接成功后播放一段 TTS 测试语音；
 * 5. 进入低频空闲循环，保持系统运行。
 */
void app_main(void)
{
    char connected_ssid[33] = {0};

    ESP_LOGI(TAG, "System start");

    ESP_ERROR_CHECK(wifi_manager_init());

    ESP_LOGI(TAG, "WiFi connect task start");
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected, SSID: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }

    esp_err_t tts_err = doubao_tts_init(main_tts_pcm_output, NULL);
    if (tts_err == ESP_OK) {
        run_tts_smoke_test();
    } else {
        ESP_LOGE(TAG, "Doubao TTS init failed: %s", esp_err_to_name(tts_err));
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}
