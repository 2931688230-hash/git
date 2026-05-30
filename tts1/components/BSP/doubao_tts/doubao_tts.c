#include "doubao_tts.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mp3_decoder.h"

static const char *TAG = "doubao_tts";

typedef enum {
    DOUBAO_TTS_RESPONSE_MODE_UNKNOWN = 0,
    DOUBAO_TTS_RESPONSE_MODE_TEXT_STREAM,
    DOUBAO_TTS_RESPONSE_MODE_RAW_MP3,
} doubao_tts_response_mode_t;

typedef struct {
    const char *text;
    RingbufHandle_t mp3_ringbuf;
    SemaphoreHandle_t http_done;
    SemaphoreHandle_t decoder_done;
    volatile bool download_done;
    volatile bool abort_requested;
    esp_err_t http_result;
    esp_err_t decoder_result;
    size_t http_bytes_read;
    uint32_t http_read_chunks;
    uint32_t http_empty_reads;
    size_t mp3_bytes_sent;
    uint32_t mp3_send_chunks;
    uint32_t mp3_send_timeouts;
    uint32_t mp3_recv_chunks;
    uint32_t mp3_recv_timeouts;
    uint32_t mp3_empty_buffers;
    uint32_t base64_chunks;
    size_t base64_input_bytes;
    size_t base64_output_bytes;
    uint32_t pcm_chunks;
    uint32_t pcm_empty_chunks;
    size_t pcm_samples;
    int64_t last_pcm_output_us;
    int64_t max_pcm_output_gap_us;
    int64_t total_pcm_output_gap_us;
    uint32_t pcm_output_gap_count;
    int64_t max_write_audio_us;
} doubao_tts_job_t;

typedef struct {
    doubao_tts_job_t *job;
    doubao_tts_response_mode_t mode;
    char *line_buf;
    size_t line_len;
    int json_depth;
    bool in_json_string;
    bool json_escape;
    bool remote_done;
} doubao_tts_stream_parser_t;

static doubao_tts_pcm_output_cb_t s_pcm_output;
static void *s_pcm_output_ctx;
static SemaphoreHandle_t s_play_lock;

/**
 * @brief 判断字符串是否为空。
 *
 * 调用方法：
 * - 只在本文件内部检查宏配置、文本参数和可选 Header 时使用；
 * - 返回 true 表示传入的是 NULL 或第一个字符就是 '\0'。
 */
static bool is_empty_string(const char *str)
{
    return str == NULL || str[0] == '\0';
}

/**
 * @brief 把毫秒转换成 FreeRTOS tick。
 *
 * 调用方法：
 * - 所有需要等待的地方统一调用本函数；
 * - 如果宏配置为 0，则表示不等待。
 */
static TickType_t ms_to_ticks(uint32_t timeout_ms)
{
    return pdMS_TO_TICKS(timeout_ms);
}

static void log_tts_stream_summary(const doubao_tts_job_t *job, esp_err_t result)
{
    if (job == NULL) {
        return;
    }

    const int64_t avg_gap_us = job->pcm_output_gap_count == 0 ? 0 :
                               job->total_pcm_output_gap_us / job->pcm_output_gap_count;
    ESP_LOGI(TAG,
             "audio_chain summary: result=%s, http_bytes=%zu, http_chunks=%u, http_empty_reads=%u, "
             "base64_chunks=%u, base64_input_bytes=%zu, base64_output_bytes=%zu, "
             "mp3_sent_bytes=%zu, mp3_send_chunks=%u, mp3_recv_chunks=%u, "
             "pcm_chunks=%u, pcm_samples=%zu, pcm_empty_chunks=%u, recv_timeouts=%u, send_timeouts=%u, "
             "empty_buffers=%u, max_write_audio_us=%lld, avg_write_interval_us=%lld, max_write_interval_us=%lld",
             esp_err_to_name(result),
             job->http_bytes_read,
             (unsigned int)job->http_read_chunks,
             (unsigned int)job->http_empty_reads,
             (unsigned int)job->base64_chunks,
             job->base64_input_bytes,
             job->base64_output_bytes,
             job->mp3_bytes_sent,
             (unsigned int)job->mp3_send_chunks,
             (unsigned int)job->mp3_recv_chunks,
             (unsigned int)job->pcm_chunks,
             job->pcm_samples,
             (unsigned int)job->pcm_empty_chunks,
             (unsigned int)job->mp3_recv_timeouts,
             (unsigned int)job->mp3_send_timeouts,
             (unsigned int)job->mp3_empty_buffers,
             (long long)job->max_write_audio_us,
             (long long)avg_gap_us,
             (long long)job->max_pcm_output_gap_us);
}

/**
 * @brief 不区分大小写地判断 source 是否包含 needle。
 *
 * 调用方法：
 * - 用于检查 HTTP Content-Type；
 * - 不依赖 strcasestr，避免不同工具链兼容性问题。
 */
static bool string_contains_ignore_case(const char *source, const char *needle)
{
    if (is_empty_string(source) || is_empty_string(needle)) {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *p = source; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断字符串是否以指定前缀开头，忽略大小写。
 *
 * 调用方法：
 * - 解析 SSE 行时识别 data:、event:、id: 等字段；
 * - prefix 需要传入普通 ASCII 字符串。
 */
static bool starts_with_ignore_case(const char *str, const char *prefix)
{
    if (str == NULL || prefix == NULL) {
        return false;
    }

    while (*prefix != '\0') {
        if (*str == '\0' ||
            tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) {
            return false;
        }
        str++;
        prefix++;
    }

    return true;
}

/**
 * @brief 原地去掉字符串两端空白字符。
 *
 * 调用方法：
 * - 解析 SSE/JSON 行前调用；
 * - 返回值仍然指向 line 内部，不需要释放。
 */
static char *trim_ascii_space(char *line)
{
    if (line == NULL) {
        return NULL;
    }

    while (*line != '\0' && isspace((unsigned char)*line)) {
        line++;
    }

    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';

    return line;
}

/**
 * @brief 判断一段文本是否像标准 base64。
 *
 * 调用方法：
 * - JSON/SSE 中的音频片段一般是 base64 字符串；
 * - 用本函数避免把普通错误文本误当成 MP3 数据解码。
 */
static bool is_base64_text(const char *str)
{
    if (is_empty_string(str)) {
        return false;
    }

    size_t valid_len = 0;
    for (const char *p = str; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '+' || ch == '/' || ch == '=') {
            valid_len++;
            continue;
        }
        return false;
    }

    return valid_len >= 4;
}

/**
 * @brief 判断一段响应数据是否像 MP3 二进制流。
 *
 * 调用方法：
 * - HTTP 响应 Content-Type 不明确时，根据首包判断是 raw MP3 还是文本事件流；
 * - 支持 ID3 标签开头和普通 MP3 帧同步字开头。
 */
static bool is_probably_mp3_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 2) {
        return false;
    }

    if (len >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        return true;
    }

    return data[0] == 0xFF && (data[1] & 0xE0) == 0xE0;
}

/**
 * @brief 判断一段响应数据是否像文本事件流。
 *
 * 调用方法：
 * - 仅用于响应模式自动识别；
 * - JSON、SSE 的 data:/event: 行都会被归类为文本流。
 */
static bool is_probably_text_stream(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < len && isspace((unsigned char)data[offset])) {
        offset++;
    }
    if (offset >= len) {
        return true;
    }

    unsigned char ch = data[offset];
    return ch == '{' || ch == '[' || ch == ':' ||
           ch == 'd' || ch == 'D' ||
           ch == 'e' || ch == 'E' ||
           ch == 'i' || ch == 'I';
}

/**
 * @brief 把 MP3 字节写入 ringbuffer。
 *
 * 调用方法：
 * - HTTP 下载任务收到 raw MP3 或 base64 解码后的 MP3 后调用；
 * - 解码任务会同时从同一个 ringbuffer 读取，实现边下载边播放。
 */
static esp_err_t send_mp3_bytes_to_ringbuffer(doubao_tts_job_t *job,
                                              const uint8_t *data,
                                              size_t len)
{
    if (job == NULL || job->mp3_ringbuf == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        job->mp3_empty_buffers++;
        ESP_LOGW(TAG,
                 "audio_chain empty buffer: stage=mp3_ringbuf_send, empty_buffers=%u",
                 (unsigned int)job->mp3_empty_buffers);
    }

    size_t offset = 0;
    while (offset < len) {
        if (job->abort_requested) {
            return ESP_FAIL;
        }

        size_t chunk_len = len - offset;
        if (chunk_len > DOUBAO_TTS_RINGBUF_RECV_MAX_SIZE) {
            chunk_len = DOUBAO_TTS_RINGBUF_RECV_MAX_SIZE;
        }

        BaseType_t sent = xRingbufferSend(job->mp3_ringbuf,
                                          data + offset,
                                          chunk_len,
                                          ms_to_ticks(DOUBAO_TTS_RINGBUF_SEND_TIMEOUT_MS));
        if (sent != pdTRUE) {
            job->mp3_send_timeouts++;
            ESP_LOGE(TAG,
                     "audio_chain buffer overflow/timeout: stage=mp3_ringbuf_send, request_bytes=%zu, sent_chunks=%u, send_timeouts=%u",
                     chunk_len,
                     (unsigned int)job->mp3_send_chunks,
                     (unsigned int)job->mp3_send_timeouts);
            job->abort_requested = true;
            return ESP_ERR_TIMEOUT;
        }

        job->mp3_bytes_sent += chunk_len;
        job->mp3_send_chunks++;
        ESP_LOGI(TAG,
                 "audio_chain chunk: stage=mp3_ringbuf_send, chunk_id=%u, chunk_bytes=%zu, total_mp3_bytes=%zu",
                 (unsigned int)job->mp3_send_chunks,
                 chunk_len,
                 job->mp3_bytes_sent);
        offset += chunk_len;
    }

    return ESP_OK;
}

/**
 * @brief 将 base64 音频片段解码后写入 ringbuffer。
 *
 * 调用方法：
 * - 解析到 SSE/JSON 中的 audio/data 字段后调用；
 * - DOUBAO_TTS_BASE64_DECODE_BUF_SIZE 控制单次解码输出缓存大小。
 */
static esp_err_t decode_base64_audio_to_ringbuffer(doubao_tts_job_t *job,
                                                   const char *base64_audio)
{
    if (job == NULL || is_empty_string(base64_audio)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_base64_text(base64_audio)) {
        ESP_LOGE(TAG, "Audio field is not valid base64 text");
        return ESP_FAIL;
    }

    uint8_t *decode_buf = (uint8_t *)malloc(DOUBAO_TTS_BASE64_DECODE_BUF_SIZE);
    if (decode_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t base64_len = strlen(base64_audio);
    size_t offset = 0;
    size_t max_input_len = (DOUBAO_TTS_BASE64_DECODE_BUF_SIZE / 3U) * 4U;

    ESP_LOGI(TAG,
             "audio_chain chunk: stage=base64_audio_input, base64_bytes=%zu",
             base64_len);

    if (max_input_len < 4) {
        free(decode_buf);
        return ESP_ERR_INVALID_SIZE;
    }

    while (offset < base64_len) {
        size_t input_len = base64_len - offset;
        if (input_len > max_input_len) {
            input_len = max_input_len;
            input_len -= input_len % 4U;
        }
        if (input_len == 0) {
            ret = ESP_FAIL;
            break;
        }

        size_t output_len = 0;
        int err = mbedtls_base64_decode(decode_buf,
                                        DOUBAO_TTS_BASE64_DECODE_BUF_SIZE,
                                        &output_len,
                                        (const unsigned char *)base64_audio + offset,
                                        input_len);
        if (err != 0) {
            ESP_LOGE(TAG, "Decode base64 audio failed: -0x%04x", -err);
            ret = ESP_FAIL;
            break;
        }

        job->base64_chunks++;
        job->base64_input_bytes += input_len;
        job->base64_output_bytes += output_len;
        ESP_LOGI(TAG,
                 "audio_chain chunk: stage=base64_decode, chunk_id=%u, input_bytes=%zu, output_mp3_bytes=%zu, total_output_mp3_bytes=%zu",
                 (unsigned int)job->base64_chunks,
                 input_len,
                 output_len,
                 job->base64_output_bytes);

        ret = send_mp3_bytes_to_ringbuffer(job, decode_buf, output_len);
        if (ret != ESP_OK) {
            break;
        }

        offset += input_len;
    }

    free(decode_buf);
    return ret;
}

/**
 * @brief 递归查找 JSON 中可能承载音频 base64 的字符串字段。
 *
 * 调用方法：
 * - 只由 process_json_payload() 调用；
 * - 兼容 data、audio、audio_data、chunk、payload 等常见字段名；
 * - depth 用来限制递归层数，避免异常 JSON 导致过深遍历。
 */
static const char *find_audio_base64_string(const cJSON *node, int depth)
{
    if (node == NULL || depth > 4) {
        return NULL;
    }

    static const char *audio_keys[] = {
        "audio",
        "audio_data",
        "data",
        "chunk",
        "payload",
    };

    if (cJSON_IsObject(node)) {
        for (size_t i = 0; i < sizeof(audio_keys) / sizeof(audio_keys[0]); i++) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(node, audio_keys[i]);
            if (cJSON_IsString(item) && is_base64_text(item->valuestring)) {
                return item->valuestring;
            }
        }

        const cJSON *child = node->child;
        while (child != NULL) {
            const char *found = find_audio_base64_string(child, depth + 1);
            if (found != NULL) {
                return found;
            }
            child = child->next;
        }
    }

    if (cJSON_IsArray(node)) {
        int count = cJSON_GetArraySize(node);
        for (int i = 0; i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(node, i);
            const char *found = find_audio_base64_string(item, depth + 1);
            if (found != NULL) {
                return found;
            }
        }
    }

    return NULL;
}

/**
 * @brief 判断 JSON 是否表示服务端已经结束音频流。
 *
 * 调用方法：
 * - 每解析出一个 JSON 事件后调用；
 * - 兼容 done、is_end、is_final、finished 等常见结束字段。
 */
static bool json_marks_stream_done(const cJSON *root)
{
    if (root == NULL || !cJSON_IsObject(root)) {
        return false;
    }

    static const char *done_keys[] = {
        "done",
        "is_end",
        "is_final",
        "finish",
        "finished",
        "end",
    };

    for (size_t i = 0; i < sizeof(done_keys) / sizeof(done_keys[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, done_keys[i]);
        if (cJSON_IsTrue(item)) {
            return true;
        }
        if (cJSON_IsNumber(item) && item->valuedouble != 0) {
            return true;
        }
        if (cJSON_IsString(item) &&
            (strcmp(item->valuestring, "true") == 0 ||
             strcmp(item->valuestring, "1") == 0)) {
            return true;
        }
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code) && code->valueint == DOUBAO_TTS_FINISH_CODE) {
        return true;
    }

    return false;
}

/**
 * @brief 检查 JSON 是否包含服务端错误码。
 *
 * 调用方法：
 * - 每个 JSON 事件先检查错误，再提取音频；
 * - code 非 0 或 error 字段非空时返回 ESP_FAIL。
 */
static esp_err_t check_json_server_error(const cJSON *root)
{
    if (root == NULL || !cJSON_IsObject(root)) {
        return ESP_OK;
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code) && code->valuedouble != 0) {
        if (code->valueint == DOUBAO_TTS_FINISH_CODE) {
            return ESP_OK;
        }

        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGE(TAG, "Doubao TTS server code=%d, message=%s",
                 code->valueint,
                 cJSON_IsString(message) ? message->valuestring : "");
        return ESP_FAIL;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(error) && !is_empty_string(error->valuestring)) {
        ESP_LOGE(TAG, "Doubao TTS server error=%s", error->valuestring);
        return ESP_FAIL;
    }
    if (cJSON_IsObject(error)) {
        char *error_text = cJSON_PrintUnformatted(error);
        ESP_LOGE(TAG, "Doubao TTS server error=%s", error_text != NULL ? error_text : "{}");
        if (error_text != NULL) {
            cJSON_free(error_text);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 处理一段 JSON 事件文本。
 *
 * 调用方法：
 * - process_text_stream_line() 判断 payload 是 JSON 后调用；
 * - 如果 JSON 内含音频 base64，会立即解码并写入 MP3 ringbuffer。
 */
static esp_err_t process_json_payload(doubao_tts_stream_parser_t *parser,
                                      const char *payload)
{
    if (parser == NULL || parser->job == NULL || is_empty_string(payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGE(TAG, "Parse TTS stream JSON failed");
        return ESP_FAIL;
    }

    esp_err_t ret = check_json_server_error(root);
    if (ret == ESP_OK) {
        const char *audio_base64 = find_audio_base64_string(root, 0);
        if (audio_base64 != NULL) {
            ret = decode_base64_audio_to_ringbuffer(parser->job, audio_base64);
        }
    }

    if (ret == ESP_OK && json_marks_stream_done(root)) {
        parser->remote_done = true;
    }

    cJSON_Delete(root);
    return ret;
}

/**
 * @brief 处理 SSE/JSON 文本流中的一行。
 *
 * 调用方法：
 * - stream_parser_feed_text() 收到 '\n' 后调用；
 * - 支持 data: JSON、data: base64、event: done 和普通单行 JSON。
 */
static esp_err_t process_text_stream_line(doubao_tts_stream_parser_t *parser,
                                          char *line)
{
    if (parser == NULL || line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *payload = trim_ascii_space(line);
    if (is_empty_string(payload)) {
        return ESP_OK;
    }

    if (payload[0] == ':') {
        return ESP_OK;
    }

    if (starts_with_ignore_case(payload, "event:")) {
        char *event_name = trim_ascii_space(payload + strlen("event:"));
        if (string_contains_ignore_case(event_name, "done") ||
            string_contains_ignore_case(event_name, "end")) {
            parser->remote_done = true;
        }
        return ESP_OK;
    }

    if (starts_with_ignore_case(payload, "id:") ||
        starts_with_ignore_case(payload, "retry:")) {
        return ESP_OK;
    }

    if (starts_with_ignore_case(payload, "data:")) {
        payload = trim_ascii_space(payload + strlen("data:"));
    }

    if (strcmp(payload, "[DONE]") == 0) {
        parser->remote_done = true;
        return ESP_OK;
    }

    if (payload[0] == '{' || payload[0] == '[') {
        return process_json_payload(parser, payload);
    }

    if (is_base64_text(payload)) {
        return decode_base64_audio_to_ringbuffer(parser->job, payload);
    }

    ESP_LOGW(TAG, "Ignore unsupported TTS stream line: %.32s", payload);
    return ESP_OK;
}

/**
 * @brief 记录 JSON 字符串、转义和括号深度状态。
 *
 * 调用方法：
 * - stream_parser_feed_text() 每接收一个字符后调用；
 * - 用于支持“每个 HTTP chunk 就是一段完整 JSON，但不一定带换行”的返回形式。
 */
static void update_json_chunk_state(doubao_tts_stream_parser_t *parser, char ch)
{
    if (parser == NULL) {
        return;
    }

    if (parser->json_escape) {
        parser->json_escape = false;
        return;
    }

    if (parser->in_json_string) {
        if (ch == '\\') {
            parser->json_escape = true;
        } else if (ch == '"') {
            parser->in_json_string = false;
        }
        return;
    }

    if (ch == '"') {
        parser->in_json_string = true;
        return;
    }

    if (ch == '{' || ch == '[') {
        parser->json_depth++;
    } else if ((ch == '}' || ch == ']') && parser->json_depth > 0) {
        parser->json_depth--;
    }
}

/**
 * @brief 重置文本流 JSON 状态机。
 *
 * 调用方法：
 * - 每成功解析完一条 JSON/SSE 事件后调用；
 * - 防止下一条事件继承上一条的括号深度或字符串状态。
 */
static void reset_json_chunk_state(doubao_tts_stream_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->json_depth = 0;
    parser->in_json_string = false;
    parser->json_escape = false;
}

/**
 * @brief 将文本流数据按行或完整 JSON chunk 切分并解析。
 *
 * 调用方法：
 * - HTTP 下载任务收到 text/event-stream 或 JSON chunk 后调用；
 * - 支持 SSE 的换行分隔，也支持官方 HTTP Chunked 中“一个 chunk 一段 JSON”的无换行形式。
 */
static esp_err_t stream_parser_feed_text(doubao_tts_stream_parser_t *parser,
                                         const uint8_t *data,
                                         size_t len)
{
    if (parser == NULL || parser->line_buf == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '\n') {
            parser->line_buf[parser->line_len] = '\0';
            esp_err_t err = process_text_stream_line(parser, parser->line_buf);
            parser->line_len = 0;
            reset_json_chunk_state(parser);
            if (err != ESP_OK || parser->remote_done) {
                return err;
            }
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (parser->line_len + 1 >= DOUBAO_TTS_STREAM_LINE_BUF_SIZE) {
            ESP_LOGE(TAG, "TTS stream line is larger than DOUBAO_TTS_STREAM_LINE_BUF_SIZE");
            return ESP_ERR_INVALID_SIZE;
        }

        parser->line_buf[parser->line_len++] = ch;
        update_json_chunk_state(parser, ch);

        size_t first_non_space = 0;
        while (first_non_space < parser->line_len &&
               isspace((unsigned char)parser->line_buf[first_non_space])) {
            first_non_space++;
        }

        if (first_non_space < parser->line_len &&
            (parser->line_buf[first_non_space] == '{' ||
             parser->line_buf[first_non_space] == '[') &&
            parser->json_depth == 0 &&
            !parser->in_json_string &&
            !parser->json_escape) {
            parser->line_buf[parser->line_len] = '\0';
            esp_err_t err = process_text_stream_line(parser, parser->line_buf);
            parser->line_len = 0;
            reset_json_chunk_state(parser);
            if (err != ESP_OK || parser->remote_done) {
                return err;
            }
        }
    }

    return ESP_OK;
}

/**
 * @brief 输入 HTTP 响应数据并自动选择解析方式。
 *
 * 调用方法：
 * - HTTP 下载任务每读到一段响应数据就调用；
 * - 首包会根据 Content-Type 和数据特征判断 raw MP3 或文本事件流。
 */
static esp_err_t stream_parser_feed(doubao_tts_stream_parser_t *parser,
                                    const uint8_t *data,
                                    size_t len)
{
    if (parser == NULL || parser->job == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0) {
        return ESP_OK;
    }

    if (parser->mode == DOUBAO_TTS_RESPONSE_MODE_UNKNOWN) {
        if (is_probably_mp3_bytes(data, len)) {
            parser->mode = DOUBAO_TTS_RESPONSE_MODE_RAW_MP3;
        } else if (is_probably_text_stream(data, len)) {
            parser->mode = DOUBAO_TTS_RESPONSE_MODE_TEXT_STREAM;
        } else {
            parser->mode = DOUBAO_TTS_RESPONSE_MODE_RAW_MP3;
        }
    }

    if (parser->mode == DOUBAO_TTS_RESPONSE_MODE_RAW_MP3) {
        return send_mp3_bytes_to_ringbuffer(parser->job, data, len);
    }

    return stream_parser_feed_text(parser, data, len);
}

/**
 * @brief HTTP 响应结束后收尾解析剩余文本。
 *
 * 调用方法：
 * - HTTP 读循环结束后调用一次；
 * - 如果最后一段 JSON 没有换行，本函数会补一次解析。
 */
static esp_err_t stream_parser_finish(doubao_tts_stream_parser_t *parser)
{
    if (parser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (parser->mode == DOUBAO_TTS_RESPONSE_MODE_TEXT_STREAM &&
        parser->line_len > 0) {
        parser->line_buf[parser->line_len] = '\0';
        esp_err_t err = process_text_stream_line(parser, parser->line_buf);
        parser->line_len = 0;
        return err;
    }

    return ESP_OK;
}

/**
 * @brief 构造 Doubao-TTS 2.0 请求体。
 *
 * 调用方法：
 * - HTTP 下载任务开始请求前调用；
 * - 返回值由 cJSON_PrintUnformatted() 分配，调用者必须用 cJSON_free() 释放。
 */
static char *build_tts_request_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user = NULL;
    cJSON *req_params = NULL;
    cJSON *audio_params = NULL;

    if (root == NULL) {
        return NULL;
    }

    if (cJSON_AddStringToObject(root, "namespace", DOUBAO_TTS_NAMESPACE) == NULL) {
        goto fail;
    }

    user = cJSON_AddObjectToObject(root, "user");
    req_params = cJSON_AddObjectToObject(root, "req_params");
    if (user == NULL || req_params == NULL) {
        goto fail;
    }

    if (cJSON_AddStringToObject(user, "uid", DOUBAO_TTS_USER_ID) == NULL ||
        cJSON_AddStringToObject(req_params, "text", text) == NULL ||
        cJSON_AddStringToObject(req_params, "speaker", DOUBAO_TTS_VOICE_TYPE) == NULL) {
        goto fail;
    }

    audio_params = cJSON_AddObjectToObject(req_params, "audio_params");
    if (audio_params == NULL) {
        goto fail;
    }

    if (cJSON_AddStringToObject(audio_params, "format", DOUBAO_TTS_AUDIO_ENCODING) == NULL ||
        cJSON_AddNumberToObject(audio_params, "sample_rate", DOUBAO_TTS_SAMPLE_RATE_HZ) == NULL) {
        goto fail;
    }

    if (DOUBAO_TTS_BIT_RATE > 0 &&
        cJSON_AddNumberToObject(audio_params, "bit_rate", DOUBAO_TTS_BIT_RATE) == NULL) {
        goto fail;
    }
    if (DOUBAO_TTS_SPEECH_RATE != 0 &&
        cJSON_AddNumberToObject(audio_params, "speech_rate", DOUBAO_TTS_SPEECH_RATE) == NULL) {
        goto fail;
    }
    if (DOUBAO_TTS_PITCH_RATE != 0 &&
        cJSON_AddNumberToObject(audio_params, "pitch", DOUBAO_TTS_PITCH_RATE) == NULL) {
        goto fail;
    }
    if (DOUBAO_TTS_VOLUME_RATE != 0 &&
        cJSON_AddNumberToObject(audio_params, "loudness_rate", DOUBAO_TTS_VOLUME_RATE) == NULL) {
        goto fail;
    }
    if (!is_empty_string(DOUBAO_TTS_EMOTION) &&
        cJSON_AddStringToObject(audio_params, "emotion", DOUBAO_TTS_EMOTION) == NULL) {
        goto fail;
    }
    if (!is_empty_string(DOUBAO_TTS_LANGUAGE) &&
        cJSON_AddStringToObject(req_params, "language", DOUBAO_TTS_LANGUAGE) == NULL) {
        goto fail;
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;

fail:
    cJSON_Delete(root);
    return NULL;
}

/**
 * @brief 设置非空可选 HTTP Header。
 *
 * 调用方法：
 * - App ID、App Key 等账号可选 Header 通过本函数设置；
 * - value 为空字符串时不设置，避免传空 Header。
 */
static esp_err_t set_optional_header(esp_http_client_handle_t client,
                                     const char *key,
                                     const char *value)
{
    if (is_empty_string(value)) {
        return ESP_OK;
    }

    return esp_http_client_set_header(client, key, value);
}

/**
 * @brief 读取并打印 HTTP 错误响应体。
 *
 * 调用方法：
 * - 服务端返回非 2xx 状态码时调用；
 * - 只读取 DOUBAO_TTS_HTTP_ERROR_BODY_SIZE 范围内的数据，避免日志和内存失控。
 */
static void log_http_error_body(esp_http_client_handle_t client, int status_code)
{
    char *error_body = (char *)calloc(1, DOUBAO_TTS_HTTP_ERROR_BODY_SIZE + 1);
    if (error_body == NULL) {
        ESP_LOGE(TAG, "Doubao TTS HTTP status=%d", status_code);
        return;
    }

    int read_len = esp_http_client_read_response(client,
                                                 error_body,
                                                 DOUBAO_TTS_HTTP_ERROR_BODY_SIZE);
    if (read_len < 0) {
        read_len = 0;
    }
    error_body[read_len] = '\0';

    ESP_LOGE(TAG, "Doubao TTS HTTP status=%d, body=%s", status_code, error_body);
    free(error_body);
}

/**
 * @brief 执行 HTTPS POST 并把返回 MP3 流写入 ringbuffer。
 *
 * 调用方法：
 * - 只由 doubao_tts_http_task() 调用；
 * - 本函数负责 HTTP、鉴权 Header、chunked/SSE 读取和 MP3 字节投递；
 * - MP3 解码播放由另一个 FreeRTOS task 并行完成。
 */
static esp_err_t doubao_tts_http_request(doubao_tts_job_t *job)
{
    if (job == NULL || is_empty_string(job->text)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *request_body = build_tts_request_body(job->text);
    if (request_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t request_body_len = strlen(request_body);
    if (request_body_len > INT_MAX) {
        cJSON_free(request_body);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *read_buf = (uint8_t *)malloc(DOUBAO_TTS_HTTP_READ_BUF_SIZE);
    char *line_buf = (char *)malloc(DOUBAO_TTS_STREAM_LINE_BUF_SIZE);
    if (read_buf == NULL || line_buf == NULL) {
        free(read_buf);
        free(line_buf);
        cJSON_free(request_body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = DOUBAO_TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DOUBAO_TTS_HTTP_TIMEOUT_MS,
        .buffer_size = DOUBAO_TTS_HTTP_READ_BUF_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(read_buf);
        free(line_buf);
        cJSON_free(request_body);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream, application/json, audio/mpeg");
    esp_http_client_set_header(client, "X-Api-Key", DOUBAO_TTS_API_KEY);
    esp_http_client_set_header(client, "X-Api-Resource-Id", DOUBAO_TTS_RESOURCE_ID);
    ESP_GOTO_ON_ERROR(set_optional_header(client, "X-Api-App-Id", DOUBAO_TTS_APP_ID),
                      cleanup,
                      TAG,
                      "Set X-Api-App-Id failed");
    ESP_GOTO_ON_ERROR(set_optional_header(client, "X-Api-App-Key", DOUBAO_TTS_APP_KEY),
                      cleanup,
                      TAG,
                      "Set X-Api-App-Key failed");

    ESP_LOGI(TAG, "Requesting Doubao TTS MP3 stream");

    ret = esp_http_client_open(client, (int)request_body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open Doubao TTS HTTPS connection failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    int written = esp_http_client_write(client, request_body, (int)request_body_len);
    if (written < 0 || written != (int)request_body_len) {
        ESP_LOGE(TAG, "Write Doubao TTS request failed, written=%d", written);
        ret = ESP_FAIL;
        goto cleanup;
    }

    (void)esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        log_http_error_body(client, status_code);
        ret = ESP_FAIL;
        goto cleanup;
    }

    doubao_tts_stream_parser_t parser = {
        .job = job,
        .mode = DOUBAO_TTS_RESPONSE_MODE_UNKNOWN,
        .line_buf = line_buf,
        .line_len = 0,
        .json_depth = 0,
        .in_json_string = false,
        .json_escape = false,
        .remote_done = false,
    };

    while (!job->abort_requested) {
        int read_len = esp_http_client_read(client,
                                            (char *)read_buf,
                                            DOUBAO_TTS_HTTP_READ_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Read Doubao TTS response failed");
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            job->http_empty_reads++;
            ESP_LOGI(TAG,
                     "audio_chain empty buffer: stage=http_read, empty_reads=%u, abort=%d, remote_done=%d",
                     (unsigned int)job->http_empty_reads,
                     job->abort_requested ? 1 : 0,
                     parser.remote_done ? 1 : 0);
            break;
        }

        job->http_read_chunks++;
        job->http_bytes_read += (size_t)read_len;
        ESP_LOGI(TAG,
                 "audio_chain chunk: stage=http_read, chunk_id=%u, chunk_bytes=%d, total_http_bytes=%zu",
                 (unsigned int)job->http_read_chunks,
                 read_len,
                 job->http_bytes_read);

        ret = stream_parser_feed(&parser, read_buf, (size_t)read_len);
        if (ret != ESP_OK) {
            break;
        }
        if (parser.remote_done) {
            break;
        }
    }

    if (ret == ESP_OK) {
        ret = stream_parser_finish(&parser);
    }

    if (ret == ESP_OK && job->mp3_bytes_sent == 0) {
        ESP_LOGE(TAG, "Doubao TTS response finished without MP3 data");
        ret = ESP_FAIL;
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(read_buf);
    free(line_buf);
    cJSON_free(request_body);
    return ret;
}

/**
 * @brief HTTP 下载任务入口。
 *
 * 调用方法：
 * - doubao_tts_play_text() 创建本任务；
 * - 任务结束前一定会设置 download_done 并释放 http_done 信号量。
 */
static void doubao_tts_http_task(void *arg)
{
    doubao_tts_job_t *job = (doubao_tts_job_t *)arg;
    if (job != NULL) {
        job->http_result = doubao_tts_http_request(job);
        job->download_done = true;
        if (job->http_result != ESP_OK) {
            job->abort_requested = true;
        }
        if (job->http_done != NULL) {
            xSemaphoreGive(job->http_done);
        }
    }

    vTaskDelete(NULL);
}

/**
 * @brief minimp3 解码后的 PCM 输出适配器。
 *
 * 调用方法：
 * - 只由 mp3_decoder 调用；
 * - 把 mp3_decoder 的 esp_err_t 回调形式适配成 doubao_tts_init() 注册的 void 回调。
 */
static esp_err_t decoder_pcm_output_cb(const int16_t *samples,
                                       size_t sample_count,
                                       int sample_rate_hz,
                                       void *user_ctx)
{
    doubao_tts_job_t *job = (doubao_tts_job_t *)user_ctx;

    if (s_pcm_output == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sample_count == 0 || samples == NULL) {
        if (job != NULL) {
            job->pcm_empty_chunks++;
        }
        ESP_LOGW(TAG,
                 "audio_chain empty buffer: stage=pcm_callback, samples=%zu, sample_rate=%d, empty_pcm_chunks=%u",
                 sample_count,
                 sample_rate_hz,
                 job != NULL ? (unsigned int)job->pcm_empty_chunks : 0U);
    }

    const int64_t now_us = esp_timer_get_time();
    int64_t interval_us = 0;
    if (job != NULL && job->last_pcm_output_us != 0) {
        interval_us = now_us - job->last_pcm_output_us;
        job->total_pcm_output_gap_us += interval_us;
        job->pcm_output_gap_count++;
        if (interval_us > job->max_pcm_output_gap_us) {
            job->max_pcm_output_gap_us = interval_us;
        }
    }
    if (job != NULL) {
        job->last_pcm_output_us = now_us;
        job->pcm_chunks++;
        job->pcm_samples += sample_count;
    }

    ESP_LOGI(TAG,
             "audio_chain pcm_chunk: stage=tts_pcm_callback, chunk_id=%u, samples=%zu, bytes=%zu, sample_rate=%d, write_interval_us=%lld",
             job != NULL ? (unsigned int)job->pcm_chunks : 0U,
             sample_count,
             sample_count * sizeof(samples[0]),
             sample_rate_hz,
             (long long)interval_us);

    const int64_t write_start_us = esp_timer_get_time();
    s_pcm_output(samples, sample_count, sample_rate_hz, s_pcm_output_ctx);
    const int64_t write_audio_us = esp_timer_get_time() - write_start_us;
    if (job != NULL && write_audio_us > job->max_write_audio_us) {
        job->max_write_audio_us = write_audio_us;
    }
    ESP_LOGI(TAG,
             "audio_chain write_audio: chunk_id=%u, samples=%zu, bytes=%zu, elapsed_us=%lld",
             job != NULL ? (unsigned int)job->pcm_chunks : 0U,
             sample_count,
             sample_count * sizeof(samples[0]),
             (long long)write_audio_us);
    return ESP_OK;
}

/**
 * @brief MP3 解码播放任务入口。
 *
 * 调用方法：
 * - doubao_tts_play_text() 创建本任务；
 * - 任务持续从 ringbuffer 读取 MP3 字节，边收边解码边输出 PCM。
 */
static void doubao_tts_decoder_task(void *arg)
{
    doubao_tts_job_t *job = (doubao_tts_job_t *)arg;
    esp_err_t ret = ESP_OK;

    if (job == NULL || job->mp3_ringbuf == NULL) {
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    mp3_decoder_t *decoder = mp3_decoder_create(decoder_pcm_output_cb, job);
    if (decoder == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    while (true) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(job->mp3_ringbuf,
                                                          &item_size,
                                                          ms_to_ticks(DOUBAO_TTS_RINGBUF_RECV_TIMEOUT_MS),
                                                          DOUBAO_TTS_RINGBUF_RECV_MAX_SIZE);
        if (item != NULL) {
            job->mp3_recv_chunks++;
            if (item_size == 0) {
                job->mp3_empty_buffers++;
                ESP_LOGW(TAG,
                         "audio_chain empty buffer: stage=mp3_ringbuf_recv, recv_chunks=%u, empty_buffers=%u",
                         (unsigned int)job->mp3_recv_chunks,
                         (unsigned int)job->mp3_empty_buffers);
            }
            ESP_LOGI(TAG,
                     "audio_chain chunk: stage=mp3_ringbuf_recv, chunk_id=%u, chunk_bytes=%zu",
                     (unsigned int)job->mp3_recv_chunks,
                     item_size);
            ret = mp3_decoder_decode(decoder, item, item_size);
            vRingbufferReturnItem(job->mp3_ringbuf, item);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MP3 decode failed: %s", esp_err_to_name(ret));
                job->abort_requested = true;
                break;
            }
            continue;
        }

        if (job->download_done || job->abort_requested) {
            break;
        }

        job->mp3_recv_timeouts++;
        ESP_LOGW(TAG,
                 "audio_chain buffer underflow: stage=mp3_ringbuf_recv, recv_timeouts=%u, download_done=%d, abort=%d",
                 (unsigned int)job->mp3_recv_timeouts,
                 job->download_done ? 1 : 0,
                 job->abort_requested ? 1 : 0);
    }

    if (ret == ESP_OK) {
        ret = mp3_decoder_flush(decoder);
    }

    mp3_decoder_destroy(decoder);

done:
    if (job != NULL) {
        job->decoder_result = ret;
        if (ret != ESP_OK) {
            job->abort_requested = true;
        }
        if (job->decoder_done != NULL) {
            xSemaphoreGive(job->decoder_done);
        }
    }

    vTaskDelete(NULL);
}

/**
 * @brief 检查 Doubao-TTS 模块运行所需配置。
 *
 * 调用方法：
 * - doubao_tts_init() 和 doubao_tts_play_text() 都会调用；
 * - API Key 留空时直接返回 ESP_ERR_INVALID_STATE，避免发出无效请求。
 */
static esp_err_t validate_config(void)
{
    if (is_empty_string(DOUBAO_TTS_API_URL) ||
        is_empty_string(DOUBAO_TTS_API_KEY) ||
        is_empty_string(DOUBAO_TTS_RESOURCE_ID) ||
        is_empty_string(DOUBAO_TTS_VOICE_TYPE)) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/**
 * @brief 创建模块级播放互斥锁。
 *
 * 调用方法：
 * - doubao_tts_init() 内部调用；
 * - 用互斥锁限制同一时刻只播放一路 TTS，避免多个 HTTP/解码任务抢同一个 Speaker。
 */
static esp_err_t ensure_play_lock_created(void)
{
    if (s_play_lock != NULL) {
        return ESP_OK;
    }

    s_play_lock = xSemaphoreCreateMutex();
    if (s_play_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t doubao_tts_init(doubao_tts_pcm_output_cb_t pcm_output, void *user_ctx)
{
    if (pcm_output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Doubao TTS config invalid, check DOUBAO_TTS_API_KEY and related macros");
        return ret;
    }

    ret = ensure_play_lock_created();
    if (ret != ESP_OK) {
        return ret;
    }

    s_pcm_output = pcm_output;
    s_pcm_output_ctx = user_ctx;

    ESP_LOGI(TAG, "Doubao TTS client ready, voice=%s, sample_rate=%d",
             DOUBAO_TTS_VOICE_TYPE,
             DOUBAO_TTS_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t doubao_tts_play_text(const char *text)
{
    if (is_empty_string(text)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(text) > DOUBAO_TTS_TEXT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_pcm_output == NULL || s_play_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = validate_config();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_play_lock, ms_to_ticks(DOUBAO_TTS_PLAY_WAIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    doubao_tts_job_t job = {
        .text = text,
        .mp3_ringbuf = NULL,
        .http_done = NULL,
        .decoder_done = NULL,
        .download_done = false,
        .abort_requested = false,
        .http_result = ESP_FAIL,
        .decoder_result = ESP_FAIL,
        .mp3_bytes_sent = 0,
    };

    bool http_task_created = false;
    bool decoder_task_created = false;
    bool wait_timed_out = false;
    TickType_t wait_ticks = ms_to_ticks(DOUBAO_TTS_PLAY_WAIT_TIMEOUT_MS);

    job.mp3_ringbuf = xRingbufferCreate(DOUBAO_TTS_MP3_RINGBUF_SIZE,
                                        RINGBUF_TYPE_BYTEBUF);
    job.http_done = xSemaphoreCreateBinary();
    job.decoder_done = xSemaphoreCreateBinary();
    if (job.mp3_ringbuf == NULL || job.http_done == NULL || job.decoder_done == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    BaseType_t task_ok = xTaskCreate(doubao_tts_decoder_task,
                                     "doubao_mp3_dec",
                                     DOUBAO_TTS_DECODER_TASK_STACK_SIZE,
                                     &job,
                                     DOUBAO_TTS_DECODER_TASK_PRIORITY,
                                     NULL);
    if (task_ok != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    decoder_task_created = true;

    task_ok = xTaskCreate(doubao_tts_http_task,
                          "doubao_http",
                          DOUBAO_TTS_HTTP_TASK_STACK_SIZE,
                          &job,
                          DOUBAO_TTS_HTTP_TASK_PRIORITY,
                          NULL);
    if (task_ok != pdPASS) {
        job.abort_requested = true;
        job.download_done = true;
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    http_task_created = true;

    if (xSemaphoreTake(job.http_done, wait_ticks) != pdTRUE) {
        wait_timed_out = true;
        job.abort_requested = true;
        ESP_LOGE(TAG, "Wait Doubao TTS HTTP task timeout");
        xSemaphoreTake(job.http_done, portMAX_DELAY);
    }

    if (xSemaphoreTake(job.decoder_done, wait_ticks) != pdTRUE) {
        wait_timed_out = true;
        job.abort_requested = true;
        ESP_LOGE(TAG, "Wait Doubao TTS decoder task timeout");
        xSemaphoreTake(job.decoder_done, portMAX_DELAY);
    }

    if (wait_timed_out) {
        ret = ESP_ERR_TIMEOUT;
    } else if (job.http_result != ESP_OK) {
        ret = job.http_result;
    } else {
        ret = job.decoder_result;
    }

cleanup:
    if (!http_task_created && decoder_task_created) {
        job.abort_requested = true;
        job.download_done = true;
        xSemaphoreTake(job.decoder_done, portMAX_DELAY);
    }

    log_tts_stream_summary(&job, ret);

    if (job.mp3_ringbuf != NULL) {
        vRingbufferDelete(job.mp3_ringbuf);
    }
    if (job.http_done != NULL) {
        vSemaphoreDelete(job.http_done);
    }
    if (job.decoder_done != NULL) {
        vSemaphoreDelete(job.decoder_done);
    }

    xSemaphoreGive(s_play_lock);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Doubao TTS play failed: %s", esp_err_to_name(ret));
    }

    return ret;
}
