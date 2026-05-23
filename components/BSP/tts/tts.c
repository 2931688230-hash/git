#include "tts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "i2s.h"

static const char *TAG = "tts";

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    size_t data_offset;
    uint32_t data_size;
} wav_info_t;

esp_err_t tts_init(void)
{
    if (TTS_API_URL[0] == '\0' || TTS_API_KEY[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "TTS client ready");
    return ESP_OK;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool chunk_id_equals(const uint8_t *p, const char *id)
{
    return memcmp(p, id, 4) == 0;
}

static esp_err_t parse_wav_header(const uint8_t *data,
                                  size_t len,
                                  wav_info_t *info)
{
    if (data == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len < 12) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (!chunk_id_equals(data, "RIFF") || !chunk_id_equals(data + 8, "WAVE")) {
        ESP_LOGE(TAG, "Response is not a RIFF/WAVE file");
        return ESP_FAIL;
    }

    bool found_fmt = false;
    bool found_data = false;
    size_t pos = 12;

    while (pos + 8 <= len) {
        const uint8_t *chunk = data + pos;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t chunk_data = pos + 8;
        size_t chunk_end = chunk_data + chunk_size;

        if (chunk_end > len) {
            return ESP_ERR_NOT_FINISHED;
        }

        if (chunk_id_equals(chunk, "fmt ")) {
            if (chunk_size < 16) {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk");
                return ESP_FAIL;
            }

            uint16_t audio_format = read_le16(data + chunk_data);
            info->channels = read_le16(data + chunk_data + 2);
            info->sample_rate = read_le32(data + chunk_data + 4);
            info->bits_per_sample = read_le16(data + chunk_data + 14);

            if (audio_format != 1) {
                ESP_LOGE(TAG, "Unsupported WAV format: %u", audio_format);
                return ESP_FAIL;
            }

            found_fmt = true;
        } else if (chunk_id_equals(chunk, "data")) {
            info->data_offset = chunk_data;
            info->data_size = chunk_size;
            found_data = true;
        }

        if (found_fmt && found_data) {
            if (info->sample_rate != 16000 ||
                info->bits_per_sample != 16 ||
                info->channels != 1) {
                ESP_LOGE(TAG, "Unsupported WAV: %lu Hz, %u bit, %u channel",
                         (unsigned long)info->sample_rate,
                         info->bits_per_sample,
                         info->channels);
                return ESP_FAIL;
            }

            ESP_LOGI(TAG, "WAV: %lu Hz, %u bit, %u channel, %lu bytes PCM",
                     (unsigned long)info->sample_rate,
                     info->bits_per_sample,
                     info->channels,
                     (unsigned long)info->data_size);
            return ESP_OK;
        }

        pos = chunk_end + (chunk_size & 1U);
    }

    return ESP_ERR_NOT_FINISHED;
}

static char *build_tts_request_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", TTS_MODEL_NAME);
    cJSON_AddStringToObject(root, "voice", TTS_VOICE_NAME);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "response_format", "wav");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t play_pcm_chunk(const uint8_t *data,
                                size_t len,
                                size_t *played_bytes,
                                uint32_t total_pcm_size)
{
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    size_t remain = (size_t)total_pcm_size - *played_bytes;
    size_t play_len = len > remain ? remain : len;
    if (play_len == 0) {
        return ESP_OK;
    }

    i2s_play((int16_t *)data, (uint32_t)play_len);
    *played_bytes += play_len;
    return ESP_OK;
}

static esp_err_t play_wav_stream(esp_http_client_handle_t client)
{
    uint8_t *read_buf = (uint8_t *)malloc(TTS_READ_BUF_SIZE);
    uint8_t *header_buf = (uint8_t *)malloc(WAV_HEADER_MAX_SIZE);
    size_t header_len = 0;
    size_t played_bytes = 0;
    bool header_ready = false;
    wav_info_t wav = {0};
    esp_err_t ret = ESP_OK;

    if (read_buf == NULL || header_buf == NULL) {
        free(read_buf);
        free(header_buf);
        return ESP_ERR_NO_MEM;
    }

    while (true) {
        int read_len = esp_http_client_read(client, (char *)read_buf,
                                           TTS_READ_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Read TTS WAV stream failed");
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            break;
        }

        if (!header_ready) {
            if (header_len + (size_t)read_len > WAV_HEADER_MAX_SIZE) {
                ESP_LOGE(TAG, "WAV header is too large");
                ret = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }

            memcpy(header_buf + header_len, read_buf, (size_t)read_len);
            header_len += (size_t)read_len;

            esp_err_t err = parse_wav_header(header_buf, header_len, &wav);
            if (err == ESP_ERR_NOT_FINISHED) {
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Parse WAV header failed: %s", esp_err_to_name(err));
                ret = err;
                goto cleanup;
            }

            header_ready = true;
            if (header_len > wav.data_offset) {
                size_t pcm_len = header_len - wav.data_offset;
                ret = play_pcm_chunk(header_buf + wav.data_offset,
                                     pcm_len,
                                     &played_bytes,
                                     wav.data_size);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Play first PCM chunk failed: %s", esp_err_to_name(ret));
                    goto cleanup;
                }
            }
            continue;
        }

        ret = play_pcm_chunk(read_buf,
                             (size_t)read_len,
                             &played_bytes,
                             wav.data_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Play PCM chunk failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        if (played_bytes >= wav.data_size) {
            break;
        }
    }

    if (!header_ready) {
        ESP_LOGE(TAG, "No complete WAV header received");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "TTS playback done, %u/%lu bytes PCM played",
             (unsigned int)played_bytes,
             (unsigned long)wav.data_size);

cleanup:
    free(header_buf);
    free(read_buf);
    return ret;
}

static esp_err_t tts_play_text_impl(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (TTS_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "TTS API key is not set");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(tts_init(), TAG, "TTS init failed");

    char *request_body = build_tts_request_body(text);
    if (request_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char auth_header[256];
    int auth_len = snprintf(auth_header, sizeof(auth_header), "Bearer %s",
                            TTS_API_KEY);
    if (auth_len < 0 || auth_len >= (int)sizeof(auth_header)) {
        cJSON_free(request_body);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(request_body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    ESP_LOGI(TAG, "Requesting TTS WAV");

    esp_err_t err = esp_http_client_open(client, strlen(request_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Open TTS HTTP connection failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int written = esp_http_client_write(client, request_body, strlen(request_body));
    if (written < 0 || written != (int)strlen(request_body)) {
        ESP_LOGE(TAG, "Write TTS request failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    (void)esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        char error_body[256] = {0};
        int error_len = esp_http_client_read_response(client, error_body,
                                                      sizeof(error_body) - 1);
        if (error_len > 0) {
            error_body[error_len] = '\0';
        }
        ESP_LOGE(TAG, "TTS HTTP status=%d, body=%s", status_code, error_body);
        err = ESP_FAIL;
        goto cleanup;
    }

    err = play_wav_stream(client);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cJSON_free(request_body);
    return err;
}

void tts_play_text(const char *text)
{
    esp_err_t err = tts_play_text_impl(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tts_play_text failed: %s", esp_err_to_name(err));
    }
}
