/* [SPEAKER_PROJECT_CHANGE] */
#include "tts_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "llm_config.h"
#include "mbedtls/base64.h"
#include "volc_gateway_auth.h"

/* [SPEAKER_PROJECT_CHANGE] */
esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz);

/* [SPEAKER_PROJECT_CHANGE] */
enum {
    TTS_CLIENT_CONNECTED_BIT = BIT0,
    TTS_CLIENT_DONE_BIT = BIT1,
    TTS_CLIENT_ERROR_BIT = BIT2,
};

/* [SPEAKER_PROJECT_CHANGE] */
typedef struct {
    esp_websocket_client_handle_t ws;
    EventGroupHandle_t events;
    char headers[LLM_TTS_HEADERS_BUFFER_SIZE];
    char rx_buffer[LLM_TTS_RX_BUFFER_SIZE + 1U];
    size_t rx_len;
    size_t rx_expected;
    uint8_t pcm_chunk[LLM_TTS_PCM_CHUNK_SIZE];
    esp_err_t last_error;
    int last_status_code;
    bool connected;
    bool audio_received;
} tts_client_ctx_t;

/* [SPEAKER_PROJECT_CHANGE] */
static tts_client_ctx_t s_tts_client;

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_logi(const char *tag, const char *format, ...)
{
#if LLM_TTS_DEBUG_ENABLE
    va_list args;
    va_start(args, format);
    esp_log_writev(ESP_LOG_INFO, tag, format, args);
    va_end(args);
#else
    (void)tag;
    (void)format;
#endif
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_set_error(esp_err_t error)
{
    if (s_tts_client.last_error == ESP_OK) {
        s_tts_client.last_error = error;
    }
    if (s_tts_client.events != NULL) {
        xEventGroupSetBits(s_tts_client.events, TTS_CLIENT_ERROR_BIT);
    }
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_reset_rx(void)
{
    s_tts_client.rx_len = 0;
    s_tts_client.rx_expected = 0;
    s_tts_client.rx_buffer[0] = '\0';
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_send_json_text(const char *json, const char *event_name)
{
    if (json == NULL || json[0] == '\0' || event_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tts_client.ws == NULL ||
        !s_tts_client.connected ||
        !esp_websocket_client_is_connected(s_tts_client.ws)) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(s_tts_client.ws,
                                              json,
                                              (int)strlen(json),
                                              pdMS_TO_TICKS(LLM_TTS_SEND_TIMEOUT_MS));
    esp_err_t ret = sent < 0 ? ESP_FAIL : ESP_OK;
    tts_client_logi(LLM_TTS_WS_LOG_TAG,
                    "send %s result=%s\n",
                    event_name,
                    esp_err_to_name(ret));
    return ret;
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_send_session_update(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    if (root == NULL || session == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, LLM_TTS_JSON_FIELD_TYPE, LLM_TTS_EVENT_SESSION_UPDATE);
    cJSON_AddItemToObject(root, LLM_TTS_JSON_FIELD_SESSION, session);
    cJSON_AddStringToObject(session, LLM_TTS_JSON_FIELD_VOICE, LLM_TTS_VOICE);
    cJSON_AddStringToObject(session, LLM_TTS_JSON_FIELD_AUDIO_FORMAT, LLM_TTS_AUDIO_FORMAT);
    cJSON_AddNumberToObject(session, LLM_TTS_JSON_FIELD_SAMPLE_RATE, LLM_TTS_SAMPLE_RATE);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = tts_client_send_json_text(json, LLM_TTS_EVENT_SESSION_UPDATE);
    cJSON_free(json);
    return ret;
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_send_input_text_append(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, LLM_TTS_JSON_FIELD_TYPE, LLM_TTS_EVENT_INPUT_TEXT_APPEND);
    cJSON_AddStringToObject(root, LLM_TTS_JSON_FIELD_DELTA, text);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = tts_client_send_json_text(json, LLM_TTS_EVENT_INPUT_TEXT_APPEND);
    cJSON_free(json);
    return ret;
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_send_input_text_done(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, LLM_TTS_JSON_FIELD_TYPE, LLM_TTS_EVENT_INPUT_TEXT_DONE);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = tts_client_send_json_text(json, LLM_TTS_EVENT_INPUT_TEXT_DONE);
    cJSON_free(json);
    return ret;
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_play_pcm_chunk(const uint8_t *pcm, size_t pcm_len)
{
    if (pcm == NULL || pcm_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((pcm_len % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t samples = (uint32_t)(pcm_len / sizeof(int16_t));
    tts_client_logi(LLM_TTS_PCM_LOG_TAG,
                    "play pcm_bytes=%u samples=%u sample_rate=%d\n",
                    (unsigned int)pcm_len,
                    (unsigned int)samples,
                    LLM_TTS_SAMPLE_RATE);
    return audio_player_play_tts_pcm((const int16_t *)pcm,
                                     samples,
                                     LLM_TTS_SAMPLE_RATE);
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_decode_base64_to_speaker(const char *delta)
{
    if (delta == NULL || delta[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t encoded_len = strlen(delta);
    size_t max_encoded_chunk = (LLM_TTS_PCM_CHUNK_SIZE / 3U) * 4U;
    if (max_encoded_chunk < 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((encoded_len % 4U) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t offset = 0;
    while (offset < encoded_len) {
        size_t remaining = encoded_len - offset;
        size_t encoded_chunk = remaining > max_encoded_chunk ? max_encoded_chunk : remaining;
        if ((encoded_chunk % 4U) != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        size_t decoded_len = 0;
        int ret = mbedtls_base64_decode(s_tts_client.pcm_chunk,
                                        sizeof(s_tts_client.pcm_chunk),
                                        &decoded_len,
                                        (const unsigned char *)&delta[offset],
                                        encoded_chunk);
        if (ret != 0) {
            ESP_LOGW(LLM_TTS_PCM_LOG_TAG, "base64 decode failed: ret=%d", ret);
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (decoded_len > 0) {
            esp_err_t play_ret = tts_client_play_pcm_chunk(s_tts_client.pcm_chunk, decoded_len);
            if (play_ret != ESP_OK) {
                ESP_LOGW(LLM_TTS_PCM_LOG_TAG,
                         "speaker play failed: %s",
                         esp_err_to_name(play_ret));
                return play_ret;
            }
            s_tts_client.audio_received = true;
        }

        offset += encoded_chunk;
    }

    return ESP_OK;
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_handle_error_json(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, LLM_TTS_JSON_FIELD_CODE);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, LLM_TTS_JSON_FIELD_MESSAGE);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, LLM_TTS_JSON_FIELD_ERROR);

    int error_code = cJSON_IsNumber(code) ? code->valueint : 0;
    const char *message_text = cJSON_IsString(message) ? message->valuestring : NULL;
    if (message_text == NULL && cJSON_IsString(error)) {
        message_text = error->valuestring;
    } else if (message_text == NULL && cJSON_IsObject(error)) {
        const cJSON *nested = cJSON_GetObjectItemCaseSensitive(error, LLM_TTS_JSON_FIELD_MESSAGE);
        message_text = cJSON_IsString(nested) ? nested->valuestring : NULL;
    }

    ESP_LOGE(LLM_TTS_WS_LOG_TAG,
             "gateway error code=%d message=%s",
             error_code,
             message_text != NULL ? message_text : "<none>");
    tts_client_set_error(ESP_FAIL);
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_handle_payload(const char *payload, size_t payload_len)
{
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        ESP_LOGW(LLM_TTS_WS_LOG_TAG, "json parse failed payload_len=%u", (unsigned int)payload_len);
        tts_client_set_error(ESP_ERR_INVALID_RESPONSE);
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, LLM_TTS_JSON_FIELD_TYPE);
    const char *event_type = cJSON_IsString(type) ? type->valuestring : NULL;
    if (event_type == NULL) {
        tts_client_handle_error_json(root);
        cJSON_Delete(root);
        return;
    }

    tts_client_logi(LLM_TTS_WS_LOG_TAG, "recv event=%s\n", event_type);

    if (strcmp(event_type, LLM_TTS_EVENT_AUDIO_DELTA) == 0) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, LLM_TTS_JSON_FIELD_DELTA);
        if (!cJSON_IsString(delta) || delta->valuestring == NULL) {
            cJSON_Delete(root);
            tts_client_set_error(ESP_ERR_INVALID_RESPONSE);
            return;
        }

        esp_err_t ret = tts_client_decode_base64_to_speaker(delta->valuestring);
        if (ret != ESP_OK) {
            tts_client_set_error(ret);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(event_type, LLM_TTS_EVENT_AUDIO_DONE) == 0) {
        xEventGroupSetBits(s_tts_client.events, TTS_CLIENT_DONE_BIT);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(event_type, LLM_TTS_EVENT_SESSION_UPDATED) == 0) {
        cJSON_Delete(root);
        return;
    }

    if (strstr(event_type, "error") != NULL) {
        tts_client_handle_error_json(root);
    }
    cJSON_Delete(root);
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_handle_ws_data(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
        return;
    }

    if (data->payload_offset == 0) {
        tts_client_reset_rx();
        if (data->payload_len <= 0 || data->payload_len > LLM_TTS_RX_BUFFER_SIZE) {
            ESP_LOGW(LLM_TTS_WS_LOG_TAG,
                     "payload too large: %d max=%d",
                     data->payload_len,
                     LLM_TTS_RX_BUFFER_SIZE);
            tts_client_set_error(ESP_ERR_INVALID_SIZE);
            return;
        }
        s_tts_client.rx_expected = (size_t)data->payload_len;
    }

    if (s_tts_client.rx_expected == 0 ||
        (size_t)data->payload_offset != s_tts_client.rx_len ||
        s_tts_client.rx_len + (size_t)data->data_len > s_tts_client.rx_expected) {
        tts_client_reset_rx();
        tts_client_set_error(ESP_ERR_INVALID_STATE);
        return;
    }

    memcpy(&s_tts_client.rx_buffer[s_tts_client.rx_len],
           data->data_ptr,
           (size_t)data->data_len);
    s_tts_client.rx_len += (size_t)data->data_len;

    if (s_tts_client.rx_len == s_tts_client.rx_expected) {
        s_tts_client.rx_buffer[s_tts_client.rx_len] = '\0';
        tts_client_handle_payload(s_tts_client.rx_buffer, s_tts_client.rx_len);
        tts_client_reset_rx();
    }
}

/* [SPEAKER_PROJECT_CHANGE] */
static void tts_client_ws_event_handler(void *handler_args,
                                        esp_event_base_t base,
                                        int32_t event_id,
                                        void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_tts_client.connected = true;
        xEventGroupSetBits(s_tts_client.events, TTS_CLIENT_CONNECTED_BIT);
        tts_client_logi(LLM_TTS_WS_LOG_TAG, "connected\n");
        break;
    case WEBSOCKET_EVENT_DATA:
        tts_client_handle_ws_data(data);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_tts_client.connected = false;
        tts_client_logi(LLM_TTS_WS_LOG_TAG, "disconnected\n");
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_tts_client.connected = false;
        if (data != NULL) {
            s_tts_client.last_status_code = data->error_handle.esp_ws_handshake_status_code;
        }
        ESP_LOGE(LLM_TTS_WS_LOG_TAG, "transport error status=%d", s_tts_client.last_status_code);
        tts_client_set_error(ESP_FAIL);
        break;
    default:
        break;
    }
}

/* [SPEAKER_PROJECT_CHANGE] */
static esp_err_t tts_client_close(void)
{
    esp_err_t close_ret = ESP_OK;
    esp_err_t stop_ret = ESP_OK;
    esp_err_t destroy_ret = ESP_OK;

    if (s_tts_client.ws != NULL) {
        if (esp_websocket_client_is_connected(s_tts_client.ws)) {
            close_ret = esp_websocket_client_close(s_tts_client.ws,
                                                   pdMS_TO_TICKS(LLM_TTS_CLOSE_TIMEOUT_MS));
            stop_ret = esp_websocket_client_stop(s_tts_client.ws);
        }
        destroy_ret = esp_websocket_client_destroy(s_tts_client.ws);
    }

    if (s_tts_client.events != NULL) {
        vEventGroupDelete(s_tts_client.events);
    }

    memset(&s_tts_client, 0, sizeof(s_tts_client));

    if (close_ret != ESP_OK) {
        return close_ret;
    }
    if (stop_ret != ESP_OK) {
        return stop_ret;
    }
    return destroy_ret;
}

/* [SPEAKER_PROJECT_CHANGE] */
esp_err_t tts_client_speak_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tts_client.ws != NULL) {
        (void)tts_client_close();
    }
    memset(&s_tts_client, 0, sizeof(s_tts_client));
    s_tts_client.last_error = ESP_OK;
    s_tts_client.events = xEventGroupCreate();
    if (s_tts_client.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = volc_gateway_auth_build_tts_ws_headers(s_tts_client.headers,
                                                           sizeof(s_tts_client.headers));
    if (ret != ESP_OK) {
        (void)tts_client_close();
        return ret;
    }

    tts_client_logi(LLM_TTS_LOG_TAG,
                    "start model=%s voice=%s format=%s sample_rate=%d text_len=%u\n",
                    LLM_TTS_MODEL,
                    LLM_TTS_VOICE,
                    LLM_TTS_AUDIO_FORMAT,
                    LLM_TTS_SAMPLE_RATE,
                    (unsigned int)strlen(text));

    esp_websocket_client_config_t ws_config = {
        .uri = LLM_TTS_WS_URL,
        .headers = s_tts_client.headers,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 0,
        .task_name = LLM_TTS_WS_TASK_NAME,
        .task_stack = LLM_TTS_WS_TASK_STACK,
        .task_prio = LLM_TTS_WS_TASK_PRIORITY,
        .buffer_size = LLM_TTS_RX_BUFFER_SIZE,
        .network_timeout_ms = LLM_TTS_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    s_tts_client.ws = esp_websocket_client_init(&ws_config);
    if (s_tts_client.ws == NULL) {
        (void)tts_client_close();
        return ESP_ERR_NO_MEM;
    }

    ret = esp_websocket_register_events(s_tts_client.ws,
                                        WEBSOCKET_EVENT_ANY,
                                        tts_client_ws_event_handler,
                                        NULL);
    if (ret != ESP_OK) {
        (void)tts_client_close();
        return ret;
    }

    ret = esp_websocket_client_start(s_tts_client.ws);
    if (ret != ESP_OK) {
        (void)tts_client_close();
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_tts_client.events,
                                           TTS_CLIENT_CONNECTED_BIT | TTS_CLIENT_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(LLM_TTS_CONNECT_TIMEOUT_MS));
    if ((bits & TTS_CLIENT_CONNECTED_BIT) == 0) {
        ret = (bits & TTS_CLIENT_ERROR_BIT) ? s_tts_client.last_error : ESP_ERR_TIMEOUT;
        (void)tts_client_close();
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    ret = tts_client_send_session_update();
    if (ret == ESP_OK) {
        ret = tts_client_send_input_text_append(text);
    }
    if (ret == ESP_OK) {
        ret = tts_client_send_input_text_done();
    }
    if (ret != ESP_OK) {
        (void)tts_client_close();
        return ret;
    }

    bits = xEventGroupWaitBits(s_tts_client.events,
                               TTS_CLIENT_DONE_BIT | TTS_CLIENT_ERROR_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(LLM_TTS_DONE_TIMEOUT_MS));
    if (bits & TTS_CLIENT_ERROR_BIT) {
        ret = s_tts_client.last_error == ESP_OK ? ESP_FAIL : s_tts_client.last_error;
    } else if ((bits & TTS_CLIENT_DONE_BIT) == 0) {
        ret = ESP_ERR_TIMEOUT;
    } else if (!s_tts_client.audio_received) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        ret = ESP_OK;
    }

    esp_err_t close_ret = tts_client_close();
    if (ret == ESP_OK && close_ret != ESP_OK) {
        ret = close_ret;
    }

    tts_client_logi(LLM_TTS_LOG_TAG, "done result=%s\n", esp_err_to_name(ret));
    return ret;
}
