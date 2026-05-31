#include "iis.h"

#include <stdbool.h>

#include "app_debug_config.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#if !SOC_I2S_SUPPORTS_PDM_TX
#error "IIS BSP requires I2S PDM TX support"
#endif

#if !SOC_I2S_SUPPORTS_PCM2PDM
#error "IIS BSP requires hardware PCM2PDM support"
#endif

static const char *TAG = "bsp_iis";

static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_tx_enabled = false;
static SemaphoreHandle_t s_iis_mutex = NULL;

static void iis_log_values(const char *message,
                           int64_t value0,
                           int64_t value1,
                           int64_t value2,
                           int64_t value3)
{
#if APP_DEBUG_BSP_IIS_LOG
    ESP_LOGI(TAG, "%s: %lld, %lld, %lld, %lld",
             message,
             value0,
             value1,
             value2,
             value3);
#else
    (void)message;
    (void)value0;
    (void)value1;
    (void)value2;
    (void)value3;
#endif
}

static esp_err_t iis_ensure_mutex(void)
{
    if (s_iis_mutex != NULL) {
        return ESP_OK;
    }

    s_iis_mutex = xSemaphoreCreateMutex();
    return s_iis_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static unsigned long iis_calc_pdm_clock_hz(const i2s_pdm_tx_clk_config_t *clk_cfg)
{
    if (clk_cfg == NULL || clk_cfg->up_sample_fs == 0) {
        return 0;
    }

    unsigned long long ratio =
        (unsigned long long)(clk_cfg->up_sample_fp / clk_cfg->up_sample_fs);
    return (unsigned long)((unsigned long long)clk_cfg->sample_rate_hz * 64ULL * ratio);
}

static esp_err_t iis_pa_init(void)
{
    if (IIS_GPIO_PA_CTL == GPIO_NUM_NC) {
        return ESP_OK;
    }

    esp_err_t err = gpio_set_direction(IIS_GPIO_PA_CTL, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_direction(PA_CTL) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(IIS_GPIO_PA_CTL, IIS_PA_ENABLE_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(PA_CTL) failed: %s", esp_err_to_name(err));
        return err;
    }

    iis_log_values("PA_CTL enabled", IIS_GPIO_PA_CTL, IIS_PA_ENABLE_LEVEL, 0, 0);
    return ESP_OK;
}

static void iis_pa_disable_on_error(void)
{
    if (IIS_GPIO_PA_CTL != GPIO_NUM_NC) {
        (void)gpio_set_level(IIS_GPIO_PA_CTL, IIS_PA_ENABLE_LEVEL ? 0 : 1);
    }
}

static esp_err_t iis_validate_pdm_tx_config(const i2s_pdm_tx_config_t *cfg)
{
    unsigned long pdm_clock_hz = iis_calc_pdm_clock_hz(&cfg->clk_cfg);

    if (cfg->clk_cfg.sample_rate_hz != IIS_SAMPLE_RATE_HZ ||
        cfg->clk_cfg.up_sample_fp != IIS_PDM_UPSAMPLE_FP ||
        cfg->clk_cfg.up_sample_fs != IIS_PDM_UPSAMPLE_FS ||
        pdm_clock_hz < IIS_PDM_CLOCK_LOW_LIMIT_HZ ||
        cfg->slot_cfg.data_bit_width != IIS_BITS_PER_SAMPLE ||
        cfg->slot_cfg.slot_mode != IIS_PDM_SLOT_MODE ||
        cfg->slot_cfg.data_fmt != I2S_PDM_DATA_FMT_PCM ||
        cfg->gpio_cfg.clk != IIS_PDM_GPIO_CLK ||
        cfg->gpio_cfg.dout != IIS_PDM_GPIO_DATA ||
        cfg->gpio_cfg.dout2 != IIS_PDM_GPIO_DATA2) {
        iis_log_values("invalid PDM TX config",
                       cfg->clk_cfg.sample_rate_hz,
                       cfg->clk_cfg.up_sample_fp,
                       cfg->clk_cfg.up_sample_fs,
                       pdm_clock_hz);
        return ESP_ERR_INVALID_ARG;
    }

#if SOC_I2S_HW_VERSION_2
    if (cfg->slot_cfg.line_mode != I2S_PDM_TX_ONE_LINE_CODEC) {
        iis_log_values("invalid PDM TX line_mode",
                       cfg->slot_cfg.line_mode,
                       I2S_PDM_TX_ONE_LINE_CODEC,
                       0,
                       0);
        return ESP_ERR_INVALID_ARG;
    }
#endif

    return ESP_OK;
}

esp_err_t iis_init(void)
{
    esp_err_t err = iis_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_chan != NULL) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    err = iis_pa_init();
    if (err != ESP_OK) {
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(IIS_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = IIS_EFFECTIVE_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = IIS_EFFECTIVE_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;

    iis_log_values("DMA config",
                   IIS_PORT,
                   chan_cfg.dma_desc_num,
                   chan_cfg.dma_frame_num,
                   chan_cfg.dma_desc_num * chan_cfg.dma_frame_num);

    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        s_tx_chan = NULL;
        iis_pa_disable_on_error();
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(IIS_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DEFAULT_CONFIG(IIS_BITS_PER_SAMPLE,
                                                           IIS_PDM_SLOT_MODE),
        .gpio_cfg = {
            .clk = IIS_PDM_GPIO_CLK,
            .dout = IIS_PDM_GPIO_DATA,
            .dout2 = IIS_PDM_GPIO_DATA2,
            .invert_flags = {
                .clk_inv = IIS_PDM_CLK_INVERT ? true : false,
            },
        },
    };

    pdm_tx_cfg.clk_cfg.sample_rate_hz = IIS_SAMPLE_RATE_HZ;
    pdm_tx_cfg.clk_cfg.up_sample_fp = IIS_PDM_UPSAMPLE_FP;
    pdm_tx_cfg.clk_cfg.up_sample_fs = IIS_PDM_UPSAMPLE_FS;
    pdm_tx_cfg.slot_cfg.data_bit_width = IIS_BITS_PER_SAMPLE;
    pdm_tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;

    iis_log_values("final PDM clock",
                   pdm_tx_cfg.clk_cfg.sample_rate_hz,
                   pdm_tx_cfg.clk_cfg.up_sample_fp,
                   pdm_tx_cfg.clk_cfg.up_sample_fs,
                   pdm_tx_cfg.clk_cfg.bclk_div);
    iis_log_values("final PDM GPIO",
                   pdm_tx_cfg.gpio_cfg.clk,
                   pdm_tx_cfg.gpio_cfg.dout,
                   pdm_tx_cfg.gpio_cfg.dout2,
                   (int)pdm_tx_cfg.gpio_cfg.invert_flags.clk_inv);

    err = iis_validate_pdm_tx_config(&pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        iis_pa_disable_on_error();
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    err = i2s_channel_init_pdm_tx_mode(s_tx_chan, &pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        iis_pa_disable_on_error();
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_chan_info_t chan_info = {};
    err = i2s_channel_get_info(s_tx_chan, &chan_info);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        iis_pa_disable_on_error();
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    iis_log_values("driver readback",
                   chan_info.is_enabled ? 1 : 0,
                   chan_info.sclk_hz,
                   chan_info.bclk_hz,
                   chan_info.total_dma_buf_size);

    if (chan_info.bclk_hz < IIS_PDM_CLOCK_LOW_LIMIT_HZ) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        iis_pa_disable_on_error();
        xSemaphoreGive(s_iis_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_enabled = false;
#if APP_DEBUG_BSP_IIS_LOG
    ESP_LOGI(TAG, "IIS PDM TX ready; channel stopped until playback starts");
#endif
    xSemaphoreGive(s_iis_mutex);
    return ESP_OK;
}

esp_err_t iis_start(void)
{
    esp_err_t err = iis_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_enabled) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK) {
        s_tx_enabled = true;
    }
    xSemaphoreGive(s_iis_mutex);
    return err;
}

esp_err_t iis_write(const void *data,
                    size_t bytes,
                    size_t *bytes_written,
                    uint32_t timeout_ms)
{
    if (bytes_written != NULL) {
        *bytes_written = 0;
    }
    if (data == NULL || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_chan == NULL || !s_tx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_write(s_tx_chan,
                             data,
                             bytes,
                             bytes_written,
                             pdMS_TO_TICKS(timeout_ms));
}

esp_err_t iis_stop(void)
{
    if (s_iis_mutex == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_chan == NULL || !s_tx_enabled) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_disable(s_tx_chan);
    if (err == ESP_OK) {
        s_tx_enabled = false;
    }
    xSemaphoreGive(s_iis_mutex);
    return err;
}

esp_err_t iis_get_info(i2s_chan_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_get_info(s_tx_chan, out_info);
}

bool iis_is_started(void)
{
    return s_tx_enabled;
}
