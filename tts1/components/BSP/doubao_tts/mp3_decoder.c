#include "mp3_decoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"

static const char *TAG = "mp3_decoder";

struct mp3_decoder {
    mp3dec_t dec;
    mp3_decoder_pcm_cb_t pcm_cb;
    void *user_ctx;
    uint8_t input_buf[MP3_DECODER_INPUT_BUF_SIZE];
    size_t input_len;
    int sample_rate_hz;
};

/**
 * @brief 把 minimp3 输出的 PCM 转成单声道并交给上层回调。
 *
 * 调用方法：
 * - 只由 decode_available_frames() 在成功解码一帧 MP3 后调用；
 * - minimp3 对双声道输出为 LRLR 交错，本函数会做简单平均混音；
 * - 单声道输入会直接分段转发，避免额外复制过多数据。
 */
static esp_err_t output_pcm_mono(mp3_decoder_t *decoder,
                                 const int16_t *pcm,
                                 size_t samples_per_channel,
                                 int channels,
                                 int sample_rate_hz)
{
    if (decoder == NULL || pcm == NULL || decoder->pcm_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (channels == 1) {
        size_t offset = 0;
        while (offset < samples_per_channel) {
            size_t chunk_samples = samples_per_channel - offset;
            if (chunk_samples > MP3_DECODER_PCM_CHUNK_SAMPLES) {
                chunk_samples = MP3_DECODER_PCM_CHUNK_SAMPLES;
            }

            esp_err_t err = decoder->pcm_cb(pcm + offset,
                                            chunk_samples,
                                            sample_rate_hz,
                                            decoder->user_ctx);
            if (err != ESP_OK) {
                return err;
            }
            offset += chunk_samples;
        }
        return ESP_OK;
    }

    if (channels == 2) {
        int16_t mono[MP3_DECODER_PCM_CHUNK_SAMPLES];
        size_t offset = 0;

        while (offset < samples_per_channel) {
            size_t chunk_samples = samples_per_channel - offset;
            if (chunk_samples > MP3_DECODER_PCM_CHUNK_SAMPLES) {
                chunk_samples = MP3_DECODER_PCM_CHUNK_SAMPLES;
            }

            for (size_t i = 0; i < chunk_samples; i++) {
                int32_t left = pcm[(offset + i) * 2];
                int32_t right = pcm[(offset + i) * 2 + 1];
                mono[i] = (int16_t)((left + right) / 2);
            }

            esp_err_t err = decoder->pcm_cb(mono,
                                            chunk_samples,
                                            sample_rate_hz,
                                            decoder->user_ctx);
            if (err != ESP_OK) {
                return err;
            }
            offset += chunk_samples;
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Unsupported MP3 channels: %d", channels);
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 尽可能从 input_buf 中解出完整 MP3 帧。
 *
 * 调用方法：
 * - mp3_decoder_decode() 每追加一段新数据后调用；
 * - 如果帧不完整，保留缓存等待下一段数据；
 * - 如果前面有 ID3 或无效字节，minimp3 会通过 frame_offset/frame_bytes 告诉我们要跳过多少。
 */
static esp_err_t decode_available_frames(mp3_decoder_t *decoder)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (decoder->input_len > 0) {
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(&decoder->dec,
                                          decoder->input_buf,
                                          (int)decoder->input_len,
                                          pcm,
                                          &info);

        if (samples <= 0) {
            if (info.frame_bytes > 0 && (size_t)info.frame_bytes <= decoder->input_len) {
                memmove(decoder->input_buf,
                        decoder->input_buf + info.frame_bytes,
                        decoder->input_len - (size_t)info.frame_bytes);
                decoder->input_len -= (size_t)info.frame_bytes;
                continue;
            }

            break;
        }

        if (info.frame_bytes <= 0 || (size_t)info.frame_bytes > decoder->input_len) {
            break;
        }

        if (info.channels <= 0 || info.channels > MP3_DECODER_MAX_CHANNELS ||
            info.hz <= 0) {
            ESP_LOGE(TAG, "Invalid MP3 frame: channels=%d hz=%d",
                     info.channels,
                     info.hz);
            return ESP_FAIL;
        }

        decoder->sample_rate_hz = info.hz;
        esp_err_t err = output_pcm_mono(decoder,
                                        pcm,
                                        (size_t)samples,
                                        info.channels,
                                        info.hz);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PCM callback failed: %s", esp_err_to_name(err));
            return err;
        }

        memmove(decoder->input_buf,
                decoder->input_buf + info.frame_bytes,
                decoder->input_len - (size_t)info.frame_bytes);
        decoder->input_len -= (size_t)info.frame_bytes;
    }

    return ESP_OK;
}

mp3_decoder_t *mp3_decoder_create(mp3_decoder_pcm_cb_t pcm_cb, void *user_ctx)
{
    if (pcm_cb == NULL) {
        return NULL;
    }

    mp3_decoder_t *decoder = (mp3_decoder_t *)calloc(1, sizeof(mp3_decoder_t));
    if (decoder == NULL) {
        return NULL;
    }

    mp3dec_init(&decoder->dec);
    decoder->pcm_cb = pcm_cb;
    decoder->user_ctx = user_ctx;
    decoder->sample_rate_hz = 0;
    return decoder;
}

void mp3_decoder_destroy(mp3_decoder_t *decoder)
{
    free(decoder);
}

esp_err_t mp3_decoder_decode(mp3_decoder_t *decoder, const uint8_t *data, size_t len)
{
    if (decoder == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t free_len = sizeof(decoder->input_buf) - decoder->input_len;
        if (free_len == 0) {
            esp_err_t err = decode_available_frames(decoder);
            if (err != ESP_OK) {
                return err;
            }

            free_len = sizeof(decoder->input_buf) - decoder->input_len;
            if (free_len == 0) {
                ESP_LOGE(TAG, "MP3 decode buffer is full, input stream may be invalid");
                return ESP_ERR_NO_MEM;
            }
        }

        size_t copy_len = len - offset;
        if (copy_len > free_len) {
            copy_len = free_len;
        }

        memcpy(decoder->input_buf + decoder->input_len, data + offset, copy_len);
        decoder->input_len += copy_len;
        offset += copy_len;

        esp_err_t err = decode_available_frames(decoder);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t mp3_decoder_flush(mp3_decoder_t *decoder)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = decode_available_frames(decoder);
    if (err != ESP_OK) {
        return err;
    }

    if (decoder->input_len > 0) {
        ESP_LOGW(TAG, "Drop %u trailing MP3 bytes",
                 (unsigned int)decoder->input_len);
        decoder->input_len = 0;
    }

    return ESP_OK;
}
