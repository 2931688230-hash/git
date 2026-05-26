#include "i2s.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s";
static i2s_chan_handle_t s_tx_chan;

/**
 * @brief 初始化 I2S 功放使能引脚。
 *
 * 调用方法：
 * - 只由 i2s_init() 调用；
 * - 使能引脚由 BSP_I2S_GPIO_PA_CTL 宏配置。
 */
static void pa_init(void)
{
    gpio_set_direction(BSP_I2S_GPIO_PA_CTL, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_I2S_GPIO_PA_CTL, 1);
}

/**
 * @brief 初始化 I2S 标准模式输出通道。
 *
 * 调用方法：
 * - 可在系统启动时主动调用；
 * - 如果未主动调用，i2s_play() 会在首次播放前自动调用。
 */
void i2s_init(void)
{
    if (s_tx_chan != NULL) {
        return;
    }

    pa_init();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = BSP_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx_chan = NULL;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_I2S_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_I2S_GPIO_BCLK,
            .ws = BSP_I2S_GPIO_WS,
            .dout = BSP_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return;
    }

    ESP_LOGI(TAG, "I2S ready: BCLK=%d, WS=%d, DOUT=%d, PA_CTL=%d",
             BSP_I2S_GPIO_BCLK, BSP_I2S_GPIO_WS, BSP_I2S_GPIO_DOUT, BSP_I2S_GPIO_PA_CTL);
}

/**
 * @brief 按字节数阻塞写入 PCM 数据到 I2S。
 *
 * 调用方法：
 * - data 指向要播放的 PCM 数据；
 * - len 是字节数；
 * - 函数内部会自动处理分段写入，直到全部写完或出现错误。
 */
void i2s_play(const int16_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    if (s_tx_chan == NULL) {
        i2s_init();
    }
    if (s_tx_chan == NULL) {
        return;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan,
                                          (const uint8_t *)data + offset,
                                          len - offset,
                                          &bytes_written,
                                          portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
            return;
        }
        offset += bytes_written;
    }
}

/**
 * @brief 播放 PCM16 单声道样本。
 *
 * 调用方法：
 * - samples 指向 int16_t 单声道 PCM；
 * - sample_count 是样本数量，本函数会换算为字节数传给 i2s_play()。
 */
void i2s_play_pcm16_mono(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return;
    }

    i2s_play(samples, (uint32_t)(sample_count * sizeof(int16_t)));
}
