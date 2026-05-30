#include "audio_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_async_log.h"
#include "resample.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#if !SOC_I2S_SUPPORTS_PDM_TX
#error "audio_player requires I2S PDM TX support"
#endif

#if !SOC_I2S_SUPPORTS_PCM2PDM
#error "audio_player requires hardware PCM2PDM support"
#endif

static const char *TAG = "audio_player";

#define AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ 24000U
#define AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FP 960U
#define AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FS (AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ / 100U)
#define AUDIO_PLAYER_REQUIRED_PDM_GPIO_CLK GPIO_NUM_8
#define AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA GPIO_NUM_7
#define AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA2 GPIO_NUM_NC
#define AUDIO_PLAYER_REQUIRED_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_PLAYER_REQUIRED_SLOT_MODE I2S_SLOT_MODE_MONO
#define AUDIO_PLAYER_EXPECTED_PDM_CLOCK_HZ 6144000UL
#define AUDIO_PLAYER_PDM_CLOCK_LOW_LIMIT_HZ ((AUDIO_PLAYER_EXPECTED_PDM_CLOCK_HZ * 95UL) / 100UL)
#define AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US 20000LL
#define AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US 500000LL
#define AUDIO_PLAYER_PCM_CHUNK_SAMPLES 512U
#define AUDIO_PLAYER_PCM_CHUNK_BYTES (AUDIO_PLAYER_PCM_CHUNK_SAMPLES * sizeof(int16_t))
#define AUDIO_PLAYER_RING_ITEM_TYPE_PCM 1U
#define AUDIO_PLAYER_RING_ITEM_TYPE_END 2U

#ifndef AUDIO_PLAYER_RING_BUFFER_CHUNKS
#define AUDIO_PLAYER_RING_BUFFER_CHUNKS 8U
#endif

#ifndef AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS
#define AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS 1000U
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE
#define AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE 4096U
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY
#define AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY 6U
#endif

#define AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS 0U
#define AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS 1U
#define AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL 32U

static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_tx_enabled = false;
static SemaphoreHandle_t s_play_mutex = NULL;
static esp_timer_handle_t s_heap_monitor_timer = NULL;

typedef struct {
    uint32_t write_count;
    uint64_t total_block_us;
    int64_t max_block_us;
} audio_player_dma_diag_t;

static audio_player_dma_diag_t s_dma_diag = {0};

typedef struct {
    uint32_t type;
    uint32_t sequence;
    uint32_t valid_samples;
    int16_t samples[AUDIO_PLAYER_PCM_CHUNK_SAMPLES];
} audio_player_ring_item_t;

typedef struct {
    RingbufHandle_t ringbuf;
    SemaphoreHandle_t done;
    volatile bool writer_done;
    esp_err_t result;
} audio_player_stream_ctx_t;

static unsigned long audio_player_calc_pdm_clock_hz(const i2s_pdm_tx_clk_config_t *clk_cfg)
{
    if (clk_cfg == NULL || clk_cfg->up_sample_fs == 0) {
        return 0;
    }

    unsigned long long ratio =
        (unsigned long long)(clk_cfg->up_sample_fp / clk_cfg->up_sample_fs);
    return (unsigned long)((unsigned long long)clk_cfg->sample_rate_hz * 64ULL * ratio);
}

static void audio_player_log_pdm_clock_config(const char *stage,
                                              const i2s_pdm_tx_clk_config_t *clk_cfg)
{
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           stage,
                           clk_cfg->sample_rate_hz,
                           clk_cfg->up_sample_fp,
                           clk_cfg->up_sample_fs,
                           clk_cfg->bclk_div);
}

static void audio_player_log_pdm_slot_config(const char *stage,
                                             const i2s_pdm_tx_slot_config_t *slot_cfg)
{
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           stage,
                           slot_cfg->slot_mode,
                           slot_cfg->data_fmt,
                           slot_cfg->data_bit_width,
                           slot_cfg->slot_bit_width);
#if SOC_I2S_HW_VERSION_2
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "PDM slot detail",
                           slot_cfg->line_mode,
                           slot_cfg->hp_en ? 1 : 0,
                           0,
                           0);
#endif
}

static void audio_player_log_heap_state(const char *stage)
{
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           stage,
                           esp_get_free_heap_size(),
                           (int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                           0,
                           0);
}

static void audio_player_heap_monitor_timer_cb(void *arg)
{
    (void)arg;
    audio_player_log_heap_state("play_running");
}

static esp_err_t audio_player_heap_monitor_ensure_timer(void)
{
    if (s_heap_monitor_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = audio_player_heap_monitor_timer_cb,
        .name = "audio_heap_monitor",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_heap_monitor_timer);
}

static void audio_player_heap_monitor_start(void)
{
    audio_player_log_heap_state("play_start");
    if (audio_player_heap_monitor_ensure_timer() != ESP_OK) {
        return;
    }
    if (esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
    (void)esp_timer_start_periodic(s_heap_monitor_timer, AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US);
}

static void audio_player_heap_monitor_stop(void)
{
    if (s_heap_monitor_timer != NULL && esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
}

static void audio_player_dma_diag_reset(void)
{
    s_dma_diag.write_count = 0;
    s_dma_diag.total_block_us = 0;
    s_dma_diag.max_block_us = 0;
}

static void audio_player_dma_diag_record(size_t request_bytes,
                                         size_t written_bytes,
                                         int64_t elapsed_us)
{
    s_dma_diag.write_count++;
    s_dma_diag.total_block_us += (uint64_t)elapsed_us;
    if (elapsed_us > s_dma_diag.max_block_us) {
        s_dma_diag.max_block_us = elapsed_us;
    }

    if (elapsed_us > AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US) {
        audio_async_log_values(AUDIO_ASYNC_LOG_WARN,
                               TAG,
                               "DMA STARVATION DETECTED",
                               (int64_t)request_bytes,
                               (int64_t)written_bytes,
                               elapsed_us,
                               0);
    }
}

static void audio_player_dma_diag_log_summary(const char *stage)
{
    uint64_t avg_us = s_dma_diag.write_count == 0 ? 0 :
                      s_dma_diag.total_block_us / s_dma_diag.write_count;
    (void)stage;
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "DMA write summary",
                           (int64_t)s_dma_diag.write_count,
                           s_dma_diag.max_block_us,
                           (int64_t)avg_us,
                           0);
}

static void audio_player_log_check_without_abort(esp_err_t err,
                                                 const char *file,
                                                 int line,
                                                 const char *func,
                                                 const char *expr)
{
    if (err == ESP_OK) {
        return;
    }
    (void)file;
    (void)line;
    (void)func;
    audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, expr, err);
}

#define AUDIO_PLAYER_LOG_CHECK_WITHOUT_ABORT(err, expr) \
    audio_player_log_check_without_abort((err), __FILE__, __LINE__, __func__, (expr))

static esp_err_t audio_player_ensure_mutex(void)
{
    if (s_play_mutex != NULL) {
        return ESP_OK;
    }
    s_play_mutex = xSemaphoreCreateMutex();
    return s_play_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t audio_player_pa_init(void)
{
    if (AUDIO_PLAYER_GPIO_PA_CTL == GPIO_NUM_NC) {
        return ESP_OK;
    }

    esp_err_t err = gpio_set_direction(AUDIO_PLAYER_GPIO_PA_CTL, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "gpio_set_direction(PA_CTL) failed", err);
        return err;
    }
    err = gpio_set_level(AUDIO_PLAYER_GPIO_PA_CTL, AUDIO_PLAYER_PA_ENABLE_LEVEL);
    if (err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "gpio_set_level(PA_CTL) failed", err);
        return err;
    }
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "PA_CTL enabled",
                           AUDIO_PLAYER_GPIO_PA_CTL,
                           AUDIO_PLAYER_PA_ENABLE_LEVEL,
                           0,
                           0);
    return ESP_OK;
}

static void audio_player_pa_disable_on_error(void)
{
    if (AUDIO_PLAYER_GPIO_PA_CTL != GPIO_NUM_NC) {
        (void)gpio_set_level(AUDIO_PLAYER_GPIO_PA_CTL, AUDIO_PLAYER_PA_ENABLE_LEVEL ? 0 : 1);
    }
}

static esp_err_t audio_player_validate_pdm_tx_config(const i2s_pdm_tx_config_t *cfg)
{
    unsigned long pdm_clock_hz = audio_player_calc_pdm_clock_hz(&cfg->clk_cfg);
    if (cfg->clk_cfg.sample_rate_hz != AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ ||
        cfg->clk_cfg.up_sample_fp != AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FP ||
        cfg->clk_cfg.up_sample_fs != AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FS ||
        pdm_clock_hz < AUDIO_PLAYER_PDM_CLOCK_LOW_LIMIT_HZ ||
        cfg->slot_cfg.data_bit_width != AUDIO_PLAYER_REQUIRED_BITS_PER_SAMPLE ||
        cfg->slot_cfg.slot_mode != AUDIO_PLAYER_REQUIRED_SLOT_MODE ||
        cfg->slot_cfg.data_fmt != I2S_PDM_DATA_FMT_PCM ||
        cfg->gpio_cfg.clk != AUDIO_PLAYER_REQUIRED_PDM_GPIO_CLK ||
        cfg->gpio_cfg.dout != AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA ||
        cfg->gpio_cfg.dout2 != AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA2) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "invalid PDM TX config",
                               cfg->clk_cfg.sample_rate_hz,
                               cfg->clk_cfg.up_sample_fp,
                               cfg->clk_cfg.up_sample_fs,
                               pdm_clock_hz);
        return ESP_ERR_INVALID_ARG;
    }
#if SOC_I2S_HW_VERSION_2
    if (cfg->slot_cfg.line_mode != I2S_PDM_TX_ONE_LINE_CODEC) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "invalid PDM TX line_mode",
                               cfg->slot_cfg.line_mode,
                               I2S_PDM_TX_ONE_LINE_CODEC,
                               0,
                               0);
        return ESP_ERR_INVALID_ARG;
    }
#endif
    return ESP_OK;
}

static esp_err_t audio_player_start_pdm_tx_for_play(void)
{
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_enabled) {
        return ESP_OK;
    }

    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "play_start enable I2S PDM TX");
    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        AUDIO_PLAYER_LOG_CHECK_WITHOUT_ABORT(err, "i2s_channel_enable()");
        return err;
    }
    s_tx_enabled = true;
    return ESP_OK;
}

static esp_err_t audio_player_i2s_channel_stop(i2s_chan_handle_t handle)
{
    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "play_end stop PDM TX");
    return i2s_channel_disable(handle);
}

static esp_err_t audio_player_stop_pdm_tx_after_play(void)
{
    if (s_tx_chan == NULL || !s_tx_enabled) {
        return ESP_OK;
    }

    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "play_end stop without flush PCM");
    esp_err_t err = audio_player_i2s_channel_stop(s_tx_chan);
    if (err != ESP_OK) {
        AUDIO_PLAYER_LOG_CHECK_WITHOUT_ABORT(err, "audio_player_i2s_channel_stop()");
        return err;
    }
    s_tx_enabled = false;
    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "play_end PDM TX stopped");
    return ESP_OK;
}

static esp_err_t audio_player_write_bytes_dma_streaming(const void *data,
                                                        size_t total_bytes,
                                                        const char *reason)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_bytes != AUDIO_PLAYER_PCM_CHUNK_BYTES) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "reject non-fixed I2S write",
                               (int64_t)total_bytes,
                               AUDIO_PLAYER_PCM_CHUNK_BYTES,
                               0,
                               0);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    uint32_t retry_count = 0;
    int64_t total_start_us = esp_timer_get_time();

    while (offset < total_bytes) {
        size_t bytes_written = 0;
        size_t bytes_left = total_bytes - offset;
        int64_t start_us = esp_timer_get_time();
        esp_err_t err = i2s_channel_write(s_tx_chan,
                                          bytes + offset,
                                          bytes_left,
                                          &bytes_written,
                                          AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        audio_player_dma_diag_record(bytes_left, bytes_written, elapsed_us);

        if (bytes_written > 0) {
            offset += bytes_written;
        }

        if (err == ESP_OK && bytes_written > 0) {
            continue;
        }

        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytes_written == 0)) {
            retry_count++;
            if ((retry_count % AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL) == 0) {
                audio_async_log_values(AUDIO_ASYNC_LOG_WARN,
                                       TAG,
                                       "audio_i2s_write DMA not ready",
                                       retry_count,
                                       (int64_t)offset,
                                       (int64_t)total_bytes,
                                       0);
            }
            vTaskDelay(AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS);
            continue;
        }

        AUDIO_PLAYER_LOG_CHECK_WITHOUT_ABORT(err, "i2s_channel_write(nonblock)");
        return err;
    }

    (void)reason;
    (void)total_start_us;
    return ESP_OK;
}

static TickType_t audio_player_ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0xFFFFFFFFU) {
        return portMAX_DELAY;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

static esp_err_t audio_player_ring_send(const audio_player_stream_ctx_t *ctx,
                                        const audio_player_ring_item_t *item)
{
    if (ctx == NULL || ctx->ringbuf == NULL || item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            return ctx->result;
        }

        BaseType_t sent = xRingbufferSend(ctx->ringbuf,
                                          item,
                                          sizeof(*item),
                                          audio_player_ms_to_ticks(AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS));
        if (sent == pdTRUE) {
            return ESP_OK;
        }

        audio_async_log_values(AUDIO_ASYNC_LOG_WARN,
                               TAG,
                               "audio ringbuffer waiting",
                               item->type,
                               item->sequence,
                               item->valid_samples,
                               0);
    }
}

static esp_err_t audio_player_ring_send_end(const audio_player_stream_ctx_t *ctx,
                                            uint32_t sequence)
{
    audio_player_ring_item_t item = {
        .type = AUDIO_PLAYER_RING_ITEM_TYPE_END,
        .sequence = sequence,
        .valid_samples = 0,
    };
    return audio_player_ring_send(ctx, &item);
}

static void audio_player_i2s_writer_task(void *arg)
{
    audio_player_stream_ctx_t *ctx = (audio_player_stream_ctx_t *)arg;
    esp_err_t result = ESP_OK;

    if (ctx == NULL || ctx->ringbuf == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t item_size = 0;
        audio_player_ring_item_t *item =
            (audio_player_ring_item_t *)xRingbufferReceive(ctx->ringbuf,
                                                           &item_size,
                                                           portMAX_DELAY);
        if (item == NULL) {
            result = result == ESP_OK ? ESP_ERR_TIMEOUT : result;
            break;
        }

        bool end_of_stream = false;
        esp_err_t item_result = ESP_OK;

        if (item_size != sizeof(*item)) {
            audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                                   TAG,
                                   "audio ringbuffer item size mismatch",
                                   (int64_t)item_size,
                                   (int64_t)sizeof(*item),
                                   0,
                                   0);
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_END) {
            end_of_stream = true;
            audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                                   TAG,
                                   "audio_chain control i2s_writer_end",
                                   item->sequence,
                                   0,
                                   0,
                                   0);
        } else if (item->type != AUDIO_PLAYER_RING_ITEM_TYPE_PCM) {
            audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                                   TAG,
                                   "audio ringbuffer item type invalid",
                                   item->type,
                                   item->sequence,
                                   0,
                                   0);
            item_result = ESP_ERR_INVALID_ARG;
        } else if (item->valid_samples == 0 ||
                   item->valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                                   TAG,
                                   "audio ringbuffer PCM valid_samples invalid",
                                   item->sequence,
                                   item->valid_samples,
                                   0,
                                   0);
            item_result = ESP_ERR_INVALID_SIZE;
        } else {
            if (result == ESP_OK) {
                item_result = audio_player_write_bytes_dma_streaming(item->samples,
                                                                     AUDIO_PLAYER_PCM_CHUNK_BYTES,
                                                                     "ringbuffer_pcm");
            }
        }

        vRingbufferReturnItem(ctx->ringbuf, item);

        if (result == ESP_OK && item_result != ESP_OK) {
            result = item_result;
        }
        if (end_of_stream) {
            break;
        }
    }

    ctx->result = result;
    ctx->writer_done = true;
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t audio_player_write_mono_pcm_to_ring(audio_player_stream_ctx_t *ctx,
                                                     const int16_t *data,
                                                     uint32_t samples,
                                                     const char *reason)
{
    uint32_t offset_samples = 0;
    uint32_t sequence = 0;

    while (offset_samples < samples) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            return ctx->result;
        }

        uint32_t valid_samples = samples - offset_samples;
        if (valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            valid_samples = AUDIO_PLAYER_PCM_CHUNK_SAMPLES;
        }

        audio_player_ring_item_t item = {
            .type = AUDIO_PLAYER_RING_ITEM_TYPE_PCM,
            .sequence = sequence,
            .valid_samples = valid_samples,
        };

        memcpy(item.samples,
               &data[offset_samples],
               (size_t)valid_samples * sizeof(item.samples[0]));
        if (valid_samples < AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            memset(&item.samples[valid_samples],
                   0,
                   (size_t)(AUDIO_PLAYER_PCM_CHUNK_SAMPLES - valid_samples) *
                   sizeof(item.samples[0]));
        }

        esp_err_t send_err = audio_player_ring_send(ctx, &item);
        if (send_err != ESP_OK) {
            audio_async_log_err(AUDIO_ASYNC_LOG_ERROR,
                                TAG,
                                "send PCM chunk to audio ringbuffer failed",
                                send_err);
            return send_err;
        }

        offset_samples += valid_samples;
        sequence++;
    }

    (void)reason;

    esp_err_t end_err = audio_player_ring_send_end(ctx, sequence);
    if (end_err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR,
                            TAG,
                            "send audio ringbuffer EOS failed",
                            end_err);
        return end_err;
    }
    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    (void)audio_async_log_init();
    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "audio_player_init");
    if (s_tx_chan != NULL) {
        return ESP_OK;
    }

    esp_err_t err = audio_player_ensure_mutex();
    if (err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "create play mutex failed", err);
        return err;
    }
    err = audio_player_pa_init();
    if (err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "PA init failed", err);
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_PLAYER_I2S_PORT,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;

    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "DMA config",
                           AUDIO_PLAYER_I2S_PORT,
                           chan_cfg.dma_desc_num,
                           chan_cfg.dma_frame_num,
                           chan_cfg.dma_desc_num * chan_cfg.dma_frame_num);

    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        s_tx_chan = NULL;
        audio_player_pa_disable_on_error();
        return err;
    }

    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DEFAULT_CONFIG(AUDIO_PLAYER_REQUIRED_BITS_PER_SAMPLE,
                                                           AUDIO_PLAYER_REQUIRED_SLOT_MODE),
        .gpio_cfg = {
            .clk = AUDIO_PLAYER_REQUIRED_PDM_GPIO_CLK,
            .dout = AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA,
            .dout2 = AUDIO_PLAYER_REQUIRED_PDM_GPIO_DATA2,
            .invert_flags = {
                .clk_inv = AUDIO_PLAYER_PDM_CLK_INVERT ? true : false,
            },
        },
    };

    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "using I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG");
    audio_player_log_pdm_clock_config("DAC default before lock", &pdm_tx_cfg.clk_cfg);
    pdm_tx_cfg.clk_cfg.sample_rate_hz = AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ;
    pdm_tx_cfg.clk_cfg.up_sample_fp = AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FP;
    pdm_tx_cfg.clk_cfg.up_sample_fs = AUDIO_PLAYER_REQUIRED_PDM_UPSAMPLE_FS;
    pdm_tx_cfg.slot_cfg.data_bit_width = AUDIO_PLAYER_REQUIRED_BITS_PER_SAMPLE;
    pdm_tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;

    audio_player_log_pdm_clock_config("final pass to i2s_channel_init_pdm_tx_mode()", &pdm_tx_cfg.clk_cfg);
    audio_player_log_pdm_slot_config("final pass to i2s_channel_init_pdm_tx_mode()", &pdm_tx_cfg.slot_cfg);
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "final GPIO pass",
                           pdm_tx_cfg.gpio_cfg.clk,
                           pdm_tx_cfg.gpio_cfg.dout,
                           pdm_tx_cfg.gpio_cfg.dout2,
                           (int)pdm_tx_cfg.gpio_cfg.invert_flags.clk_inv);

    err = audio_player_validate_pdm_tx_config(&pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        audio_player_pa_disable_on_error();
        return err;
    }

    err = i2s_channel_init_pdm_tx_mode(s_tx_chan, &pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        audio_player_pa_disable_on_error();
        return err;
    }

    i2s_chan_info_t chan_info = {};
    err = i2s_channel_get_info(s_tx_chan, &chan_info);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        audio_player_pa_disable_on_error();
        return err;
    }

    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "driver readback",
                           chan_info.is_enabled ? 1 : 0,
                           chan_info.sclk_hz,
                           chan_info.bclk_hz,
                           chan_info.total_dma_buf_size);
    if (chan_info.bclk_hz < AUDIO_PLAYER_PDM_CLOCK_LOW_LIMIT_HZ) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        audio_player_pa_disable_on_error();
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_enabled = false;
    audio_async_log_text(AUDIO_ASYNC_LOG_INFO,
                         TAG,
                         "audio_player ready; PDM TX stopped until playback starts");
    return ESP_OK;
}

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples)
{
    (void)audio_async_log_init();
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "audio_player_play_pcm",
                           samples,
                           0,
                           0,
                           0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)samples > SIZE_MAX / sizeof(data[0])) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pcm_bytes = (size_t)samples * sizeof(data[0]);
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "PDM TX write format",
                           AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                           (int64_t)pcm_bytes,
                           ((int64_t)samples * 1000LL) / AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                           0);

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_player_dma_diag_reset();
    audio_player_heap_monitor_start();

    i2s_chan_info_t play_chan_info = {};
    if (i2s_channel_get_info(s_tx_chan, &play_chan_info) == ESP_OK) {
        audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                               TAG,
                               "play_start DMA diagnostic",
                               AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM,
                               AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM,
                               play_chan_info.total_dma_buf_size,
                               0);
    }

    audio_player_stream_ctx_t stream_ctx = {
        .ringbuf = NULL,
        .done = NULL,
        .writer_done = false,
        .result = ESP_OK,
    };
    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;

    stream_ctx.ringbuf = xRingbufferCreateNoSplit(ring_item_size, ring_item_count);
    if (stream_ctx.ringbuf == NULL) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "create audio ringbuffer failed",
                               (int64_t)ring_item_size,
                               (int64_t)ring_item_count,
                               0,
                               0);
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    stream_ctx.done = xSemaphoreCreateBinary();
    if (stream_ctx.done == NULL) {
        audio_async_log_text(AUDIO_ASYNC_LOG_ERROR,
                             TAG,
                             "create audio writer done semaphore failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "audio ringbuffer ready",
                           AUDIO_PLAYER_PCM_CHUNK_SAMPLES,
                           (int64_t)ring_item_size,
                           (int64_t)ring_item_count,
                           0);

    err = audio_player_start_pdm_tx_for_play();
    if (err != ESP_OK) {
        goto play_cleanup;
    }

    BaseType_t task_created = xTaskCreate(audio_player_i2s_writer_task,
                                          "audio_i2s_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        audio_async_log_text(AUDIO_ASYNC_LOG_ERROR,
                             TAG,
                             "create audio_i2s_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    err = audio_player_write_mono_pcm_to_ring(&stream_ctx, data, samples, "PCM");
    if (err != ESP_OK) {
        (void)audio_player_ring_send_end(&stream_ctx, 0);
    }

    if (xSemaphoreTake(stream_ctx.done, portMAX_DELAY) != pdTRUE) {
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && stream_ctx.result != ESP_OK) {
        err = stream_ctx.result;
    }

play_cleanup:
    esp_err_t stop_err = audio_player_stop_pdm_tx_after_play();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }

    if (stream_ctx.ringbuf != NULL) {
        vRingbufferDelete(stream_ctx.ringbuf);
    }
    if (stream_ctx.done != NULL) {
        vSemaphoreDelete(stream_ctx.done);
    }

    audio_player_dma_diag_log_summary("play_end");
    audio_player_heap_monitor_stop();
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "PCM playback failed", err);
        return err;
    }
    audio_async_log_text(AUDIO_ASYNC_LOG_INFO, TAG, "playback complete; no post-play PCM output");
    return ESP_OK;
}

esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz)
{
    (void)audio_async_log_init();
    audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                           TAG,
                           "audio_player_play_tts_pcm",
                           sample_rate_hz,
                           samples,
                           AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                           0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == (int)AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ) {
        audio_async_log_text(AUDIO_ASYNC_LOG_INFO,
                             TAG,
                             "TTS PCM already matches audio_player output rate");
        return audio_player_play_pcm(data, samples);
    }
    if (sample_rate_hz != AUDIO_RESAMPLE_16K_HZ) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "unsupported TTS PCM sample rate",
                               sample_rate_hz,
                               AUDIO_RESAMPLE_16K_HZ,
                               AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                               0);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t output_samples = audio_resample_16k_to_24k_output_samples(samples);
    if (output_samples == 0 ||
        output_samples > UINT32_MAX ||
        output_samples > SIZE_MAX / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t *resampled = (int16_t *)heap_caps_malloc(output_samples * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
    if (resampled == NULL) {
        audio_async_log_values(AUDIO_ASYNC_LOG_ERROR,
                               TAG,
                               "TTS resample alloc failed",
                               samples,
                               (int64_t)output_samples,
                               0,
                               0);
        return ESP_ERR_NO_MEM;
    }

    size_t produced_samples = 0;
    esp_err_t err = audio_resample_16k_to_24k_linear(data,
                                                     samples,
                                                     resampled,
                                                     output_samples,
                                                     &produced_samples);
    if (err == ESP_OK) {
        audio_async_log_values(AUDIO_ASYNC_LOG_INFO,
                               TAG,
                               "TTS resample",
                               sample_rate_hz,
                               AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                               samples,
                               (int64_t)produced_samples);
        err = audio_player_play_pcm(resampled, (uint32_t)produced_samples);
    } else {
        audio_async_log_err(AUDIO_ASYNC_LOG_ERROR, TAG, "TTS resample failed", err);
    }

    heap_caps_free(resampled);
    return err;
}
