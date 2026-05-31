#include "speaker_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_debug_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iis.h"
#include "speaker_resample.h"

static const char *TAG = "speaker_player";

#define AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ
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

static void speaker_player_log_values(const char *message,
                                      int64_t value0,
                                      int64_t value1,
                                      int64_t value2,
                                      int64_t value3)
{
#if APP_DEBUG_SPEAKER_PLAYER_LOG
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

static void speaker_player_log_heap_state(const char *stage)
{
    speaker_player_log_values(stage,
                              esp_get_free_heap_size(),
                              (int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              0,
                              0);
}

static void speaker_player_heap_monitor_timer_cb(void *arg)
{
    (void)arg;
    speaker_player_log_heap_state("play_running");
}

static esp_err_t speaker_player_heap_monitor_ensure_timer(void)
{
    if (s_heap_monitor_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = speaker_player_heap_monitor_timer_cb,
        .name = "speaker_heap_monitor",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_heap_monitor_timer);
}

static void speaker_player_heap_monitor_start(void)
{
    speaker_player_log_heap_state("play_start");
    if (speaker_player_heap_monitor_ensure_timer() != ESP_OK) {
        return;
    }
    if (esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
    (void)esp_timer_start_periodic(s_heap_monitor_timer,
                                   AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US);
}

static void speaker_player_heap_monitor_stop(void)
{
    if (s_heap_monitor_timer != NULL && esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
}

static void speaker_player_dma_diag_reset(void)
{
    s_dma_diag.write_count = 0;
    s_dma_diag.total_block_us = 0;
    s_dma_diag.max_block_us = 0;
}

static void speaker_player_dma_diag_record(size_t request_bytes,
                                           size_t written_bytes,
                                           int64_t elapsed_us)
{
    s_dma_diag.write_count++;
    s_dma_diag.total_block_us += (uint64_t)elapsed_us;
    if (elapsed_us > s_dma_diag.max_block_us) {
        s_dma_diag.max_block_us = elapsed_us;
    }

    if (elapsed_us > AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US) {
        ESP_LOGW(TAG,
                 "DMA starvation: request=%zu written=%zu elapsed_us=%lld",
                 request_bytes,
                 written_bytes,
                 elapsed_us);
    }
}

static void speaker_player_dma_diag_log_summary(void)
{
    uint64_t avg_us = s_dma_diag.write_count == 0 ? 0 :
                      s_dma_diag.total_block_us / s_dma_diag.write_count;
    speaker_player_log_values("DMA write summary",
                              (int64_t)s_dma_diag.write_count,
                              s_dma_diag.max_block_us,
                              (int64_t)avg_us,
                              0);
}

static esp_err_t speaker_player_ensure_mutex(void)
{
    if (s_play_mutex != NULL) {
        return ESP_OK;
    }
    s_play_mutex = xSemaphoreCreateMutex();
    return s_play_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t speaker_player_write_bytes_dma_streaming(const void *data,
                                                          size_t total_bytes)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_bytes != AUDIO_PLAYER_PCM_CHUNK_BYTES) {
        ESP_LOGE(TAG,
                 "reject non-fixed IIS write: bytes=%zu expected=%zu",
                 total_bytes,
                 (size_t)AUDIO_PLAYER_PCM_CHUNK_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    uint32_t retry_count = 0;

    while (offset < total_bytes) {
        size_t bytes_written = 0;
        size_t bytes_left = total_bytes - offset;
        int64_t start_us = esp_timer_get_time();
        esp_err_t err = iis_write(bytes + offset,
                                  bytes_left,
                                  &bytes_written,
                                  AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        speaker_player_dma_diag_record(bytes_left, bytes_written, elapsed_us);

        if (bytes_written > 0) {
            offset += bytes_written;
        }

        if (err == ESP_OK && bytes_written > 0) {
            continue;
        }

        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytes_written == 0)) {
            retry_count++;
            if ((retry_count % AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG,
                         "IIS DMA not ready: retries=%lu offset=%zu total=%zu",
                         (unsigned long)retry_count,
                         offset,
                         total_bytes);
            }
            vTaskDelay(AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS);
            continue;
        }

        ESP_LOGE(TAG, "iis_write failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static TickType_t speaker_player_ms_to_ticks(uint32_t timeout_ms)
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

static esp_err_t speaker_player_ring_send(const audio_player_stream_ctx_t *ctx,
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
                                          speaker_player_ms_to_ticks(AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS));
        if (sent == pdTRUE) {
            return ESP_OK;
        }

#if APP_DEBUG_SPEAKER_PLAYER_LOG
        ESP_LOGW(TAG,
                 "speaker ringbuffer waiting: type=%lu seq=%lu samples=%lu",
                 (unsigned long)item->type,
                 (unsigned long)item->sequence,
                 (unsigned long)item->valid_samples);
#endif
    }
}

static esp_err_t speaker_player_ring_send_end(const audio_player_stream_ctx_t *ctx,
                                              uint32_t sequence)
{
    audio_player_ring_item_t item = {
        .type = AUDIO_PLAYER_RING_ITEM_TYPE_END,
        .sequence = sequence,
        .valid_samples = 0,
    };
    return speaker_player_ring_send(ctx, &item);
}

static void speaker_player_iis_writer_task(void *arg)
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
            ESP_LOGE(TAG,
                     "speaker ringbuffer item size mismatch: got=%zu expected=%zu",
                     item_size,
                     sizeof(*item));
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_END) {
            end_of_stream = true;
            speaker_player_log_values("speaker writer end", item->sequence, 0, 0, 0);
        } else if (item->type != AUDIO_PLAYER_RING_ITEM_TYPE_PCM) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item type invalid: type=%lu seq=%lu",
                     (unsigned long)item->type,
                     (unsigned long)item->sequence);
            item_result = ESP_ERR_INVALID_ARG;
        } else if (item->valid_samples == 0 ||
                   item->valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            ESP_LOGE(TAG,
                     "speaker PCM valid_samples invalid: seq=%lu samples=%lu",
                     (unsigned long)item->sequence,
                     (unsigned long)item->valid_samples);
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (result == ESP_OK) {
            item_result = speaker_player_write_bytes_dma_streaming(item->samples,
                                                                   AUDIO_PLAYER_PCM_CHUNK_BYTES);
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

static esp_err_t speaker_player_write_mono_pcm_to_ring(audio_player_stream_ctx_t *ctx,
                                                       const int16_t *data,
                                                       uint32_t samples)
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

        esp_err_t send_err = speaker_player_ring_send(ctx, &item);
        if (send_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send PCM chunk to speaker ringbuffer failed: %s",
                     esp_err_to_name(send_err));
            return send_err;
        }

        offset_samples += valid_samples;
        sequence++;
    }

    esp_err_t end_err = speaker_player_ring_send_end(ctx, sequence);
    if (end_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "send speaker ringbuffer EOS failed: %s",
                 esp_err_to_name(end_err));
        return end_err;
    }
    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    esp_err_t err = speaker_player_ensure_mutex();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create play mutex failed: %s", esp_err_to_name(err));
        return err;
    }

    return iis_init();
}

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples)
{
    speaker_player_log_values("audio_player_play_pcm", samples, 0, 0, 0);

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
    speaker_player_log_values("PDM TX write format",
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

    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();

    i2s_chan_info_t play_chan_info = {};
    if (iis_get_info(&play_chan_info) == ESP_OK) {
        speaker_player_log_values("play_start DMA diagnostic",
                                  IIS_EFFECTIVE_DMA_DESC_NUM,
                                  IIS_EFFECTIVE_DMA_FRAME_NUM,
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
        ESP_LOGE(TAG,
                 "create speaker ringbuffer failed: item_size=%zu item_count=%zu",
                 ring_item_size,
                 ring_item_count);
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    stream_ctx.done = xSemaphoreCreateBinary();
    if (stream_ctx.done == NULL) {
        ESP_LOGE(TAG, "create speaker writer done semaphore failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    speaker_player_log_values("speaker ringbuffer ready",
                              AUDIO_PLAYER_PCM_CHUNK_SAMPLES,
                              (int64_t)ring_item_size,
                              (int64_t)ring_item_count,
                              0);

    err = iis_start();
    if (err != ESP_OK) {
        goto play_cleanup;
    }

    BaseType_t task_created = xTaskCreate(speaker_player_iis_writer_task,
                                          "speaker_iis_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "create speaker_iis_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    err = speaker_player_write_mono_pcm_to_ring(&stream_ctx, data, samples);
    if (err != ESP_OK) {
        (void)speaker_player_ring_send_end(&stream_ctx, 0);
    }

    if (xSemaphoreTake(stream_ctx.done, portMAX_DELAY) != pdTRUE) {
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && stream_ctx.result != ESP_OK) {
        err = stream_ctx.result;
    }

play_cleanup:
    esp_err_t stop_err = iis_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }

    if (stream_ctx.ringbuf != NULL) {
        vRingbufferDelete(stream_ctx.ringbuf);
    }
    if (stream_ctx.done != NULL) {
        vSemaphoreDelete(stream_ctx.done);
    }

    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(err));
        return err;
    }
    speaker_player_log_values("playback complete", samples, 0, 0, 0);
    return ESP_OK;
}

esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz)
{
    speaker_player_log_values("audio_player_play_tts_pcm",
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
        return audio_player_play_pcm(data, samples);
    }
    if (sample_rate_hz != AUDIO_RESAMPLE_16K_HZ) {
        ESP_LOGE(TAG,
                 "unsupported PCM sample rate: got=%d supported=%d,%u",
                 sample_rate_hz,
                 AUDIO_RESAMPLE_16K_HZ,
                 AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
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
        ESP_LOGE(TAG,
                 "PCM resample alloc failed: in_samples=%lu out_samples=%zu",
                 (unsigned long)samples,
                 output_samples);
        return ESP_ERR_NO_MEM;
    }

    size_t produced_samples = 0;
    esp_err_t err = audio_resample_16k_to_24k_linear(data,
                                                     samples,
                                                     resampled,
                                                     output_samples,
                                                     &produced_samples);
    if (err == ESP_OK) {
        speaker_player_log_values("PCM resample",
                                  sample_rate_hz,
                                  AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                                  samples,
                                  (int64_t)produced_samples);
        err = audio_player_play_pcm(resampled, (uint32_t)produced_samples);
    } else {
        ESP_LOGE(TAG, "PCM resample failed: %s", esp_err_to_name(err));
    }

    heap_caps_free(resampled);
    return err;
}
