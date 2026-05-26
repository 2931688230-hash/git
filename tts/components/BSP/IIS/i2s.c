#include "i2s.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define I2S_BCLK  GPIO_NUM_8
#define I2S_WS    GPIO_NUM_7
#define PA_CTL    GPIO_NUM_1
#define I2S_DOUT  GPIO_NUM_9
#define I2S_PORT  I2S_NUM_0

static const char *TAG = "i2s";
static i2s_chan_handle_t s_tx_chan;

static void pa_init(void)
{
    gpio_set_direction(PA_CTL, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTL, 1);
}

void i2s_init(void)
{
    if (s_tx_chan != NULL) {
        return;
    }

    pa_init();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx_chan = NULL;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
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
             I2S_BCLK, I2S_WS, I2S_DOUT, PA_CTL);
}

void i2s_play(int16_t *data, uint32_t len)
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

void i2s_play_pcm16_mono(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return;
    }

    i2s_play((int16_t *)samples, (uint32_t)(sample_count * sizeof(int16_t)));
}
