#ifndef DOUBAO_TTS_H
#define DOUBAO_TTS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Doubao-TTS 2.0 模块配置区。
 *
 * 使用约定：
 * 1. 后续调试可能会改的参数全部放在本头文件，不在 doubao_tts.c 里散落硬编码；
 * 2. API Key 默认留空，避免把真实密钥写进仓库；
 * 3. 本模块只负责“联网请求 + 流式解析 + MP3 解码调度”，不直接依赖具体喇叭模块；
 * 4. 播放输出通过 doubao_tts_init() 传入 PCM 回调，方便后期从 I2S 换成 DAC、PDM 或文件保存。
 */

/* Doubao-TTS 2.0 单向流式 HTTP 接口地址；如果火山控制台文档更新域名或路径，只改这里。 */
#ifndef DOUBAO_TTS_API_URL
#define DOUBAO_TTS_API_URL "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#endif

/* Doubao-TTS 2.0 API Key；本地测试时填真实值，提交仓库前建议保持为空字符串。 */
#ifndef DOUBAO_TTS_API_KEY
#define DOUBAO_TTS_API_KEY "ark-ec498c56-96bd-44c2-b022-4630fd2c563c-a5395"
#endif

/* Doubao-TTS 2.0 资源 ID；新版鉴权要求请求头 X-Api-Resource-Id 使用 seed-tts-2.0。 */
#ifndef DOUBAO_TTS_RESOURCE_ID
#define DOUBAO_TTS_RESOURCE_ID "seed-tts-2.0"
#endif

/* Doubao-TTS V3 请求 namespace；官方默认值为 BidirectionalTTS，通常不需要修改。 */
#ifndef DOUBAO_TTS_NAMESPACE
#define DOUBAO_TTS_NAMESPACE "BidirectionalTTS"
#endif

/* 默认音色；当前按需求固定为中文女声 zh_female_vv_uranus_bigtts。 */
#ifndef DOUBAO_TTS_VOICE_TYPE
#define DOUBAO_TTS_VOICE_TYPE "zh_female_vv_uranus_bigtts"
#endif

/* 请求输出音频编码；当前链路固定请求 MP3，收到后由 minimp3 解码成 PCM。 */
#ifndef DOUBAO_TTS_AUDIO_ENCODING
#define DOUBAO_TTS_AUDIO_ENCODING "mp3"
#endif

/* 请求输出采样率，单位 Hz；当前项目 I2S 默认 16000 Hz，因此这里也默认 16000，避免播放变速。 */
#ifndef DOUBAO_TTS_SAMPLE_RATE_HZ
#define DOUBAO_TTS_SAMPLE_RATE_HZ 16000
#endif

/* 请求码率，单位 bps；0 表示使用服务端默认码率。 */
#ifndef DOUBAO_TTS_BIT_RATE
#define DOUBAO_TTS_BIT_RATE 0
#endif

/* 请求语速调节值；写入 audio_params.speech_rate，范围一般为 -50 到 100，0 表示默认语速。 */
#ifndef DOUBAO_TTS_SPEECH_RATE
#define DOUBAO_TTS_SPEECH_RATE 0
#endif

/* 请求音高调节值；写入 audio_params.pitch，0 表示默认音高，非 0 时按服务端文档范围填写。 */
#ifndef DOUBAO_TTS_PITCH_RATE
#define DOUBAO_TTS_PITCH_RATE 0
#endif

/* 请求音量调节值；写入 audio_params.loudness_rate，范围一般为 -50 到 100，0 表示默认音量。 */
#ifndef DOUBAO_TTS_VOLUME_RATE
#define DOUBAO_TTS_VOLUME_RATE 0
#endif

/* 情绪参数；写入 audio_params.emotion，空字符串表示不指定，具体可用值需要按当前音色能力填写。 */
#ifndef DOUBAO_TTS_EMOTION
#define DOUBAO_TTS_EMOTION ""
#endif

/* 语言提示；空字符串表示不指定，跨语种文本可按文档填写 zh-cn、en、ja 等。 */
#ifndef DOUBAO_TTS_LANGUAGE
#define DOUBAO_TTS_LANGUAGE ""
#endif

/* 请求体 user.uid；只用于服务端日志和调用方标识，不依赖 Wi-Fi 或设备唯一 ID。 */
#ifndef DOUBAO_TTS_USER_ID
#define DOUBAO_TTS_USER_ID "esp32-c5"
#endif

/* 可选应用 ID；如果你的账号要求 X-Api-App-Id，请在这里填写，否则保持为空。 */
#ifndef DOUBAO_TTS_APP_ID
#define DOUBAO_TTS_APP_ID ""
#endif

/* 可选应用 Key；如果你的账号要求 X-Api-App-Key，请在这里填写，否则保持为空。 */
#ifndef DOUBAO_TTS_APP_KEY
#define DOUBAO_TTS_APP_KEY ""
#endif

/* HTTP 连接和读取超时时间，单位 ms；网络慢或首包慢时可适当调大。 */
#ifndef DOUBAO_TTS_HTTP_TIMEOUT_MS
#define DOUBAO_TTS_HTTP_TIMEOUT_MS 30000
#endif

/* 每次从 HTTP 响应流读取的字节数；越小首包处理越快，越大系统调用次数越少。 */
#ifndef DOUBAO_TTS_HTTP_READ_BUF_SIZE
#define DOUBAO_TTS_HTTP_READ_BUF_SIZE 1024
#endif

/* HTTP 非 2xx 错误响应体缓存大小，单位 byte；用于日志打印服务端错误信息。 */
#ifndef DOUBAO_TTS_HTTP_ERROR_BODY_SIZE
#define DOUBAO_TTS_HTTP_ERROR_BODY_SIZE 512
#endif

/* Doubao-TTS V3 流式结束成功码；服务端返回该 code 表示合成正常结束，不应当按错误处理。 */
#ifndef DOUBAO_TTS_FINISH_CODE
#define DOUBAO_TTS_FINISH_CODE 20000000
#endif

/* MP3 ringbuffer 大小，单位 byte；太小容易下载端等待，太大会增加 RAM 占用。 */
#ifndef DOUBAO_TTS_MP3_RINGBUF_SIZE
#define DOUBAO_TTS_MP3_RINGBUF_SIZE (24 * 1024)
#endif

/* 下载任务栈大小，单位 byte；HTTPS + JSON 解析会占用较多栈。 */
#ifndef DOUBAO_TTS_HTTP_TASK_STACK_SIZE
#define DOUBAO_TTS_HTTP_TASK_STACK_SIZE 12288
#endif

/* 解码任务栈大小，单位 byte；minimp3 解码和 PCM 临时缓存需要一定栈空间。 */
#ifndef DOUBAO_TTS_DECODER_TASK_STACK_SIZE
#define DOUBAO_TTS_DECODER_TASK_STACK_SIZE 12288
#endif

/* 下载任务优先级；略高于普通业务任务即可，避免网络流被饿死。 */
#ifndef DOUBAO_TTS_HTTP_TASK_PRIORITY
#define DOUBAO_TTS_HTTP_TASK_PRIORITY 5
#endif

/* 解码播放任务优先级；建议不低于下载任务，降低播放断续概率。 */
#ifndef DOUBAO_TTS_DECODER_TASK_PRIORITY
#define DOUBAO_TTS_DECODER_TASK_PRIORITY 6
#endif

/* ringbuffer 写入等待时间，单位 ms；用于下载端等待解码端释放空间。 */
#ifndef DOUBAO_TTS_RINGBUF_SEND_TIMEOUT_MS
#define DOUBAO_TTS_RINGBUF_SEND_TIMEOUT_MS 1000
#endif

/* 解码端读取 ringbuffer 的等待时间，单位 ms；等待超时后会检查下载任务是否结束。 */
#ifndef DOUBAO_TTS_RINGBUF_RECV_TIMEOUT_MS
#define DOUBAO_TTS_RINGBUF_RECV_TIMEOUT_MS 100
#endif

/* 解码端单次最多从 ringbuffer 取出的 MP3 字节数；影响解码延迟和 memcpy 开销。 */
#ifndef DOUBAO_TTS_RINGBUF_RECV_MAX_SIZE
#define DOUBAO_TTS_RINGBUF_RECV_MAX_SIZE 1024
#endif

/* JSON/SSE 事件缓存大小，单位 byte；V3 流式响应中 data 字段是 base64 音频片段。 */
#ifndef DOUBAO_TTS_STREAM_LINE_BUF_SIZE
#define DOUBAO_TTS_STREAM_LINE_BUF_SIZE 4096
#endif

/* base64 解码临时输出缓存大小，单位 byte；一段音频片段超过此值会被分段解码写入 ringbuffer。 */
#ifndef DOUBAO_TTS_BASE64_DECODE_BUF_SIZE
#define DOUBAO_TTS_BASE64_DECODE_BUF_SIZE 1024
#endif

/* 文本请求最大长度，单位 byte；超过后拒绝请求，避免 JSON body 占用不可控内存。 */
#ifndef DOUBAO_TTS_TEXT_MAX_LEN
#define DOUBAO_TTS_TEXT_MAX_LEN 512
#endif

/* 单次播放完成等待时间，单位 ms；同步接口会在此时间内等待下载和解码任务结束。 */
#ifndef DOUBAO_TTS_PLAY_WAIT_TIMEOUT_MS
#define DOUBAO_TTS_PLAY_WAIT_TIMEOUT_MS 120000
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCM16 单声道播放回调类型。
 *
 * 调用方法：
 * - 业务层实现一个函数，例如：
 *   static void play_pcm(const int16_t *samples, size_t sample_count, int sample_rate_hz, void *user_ctx);
 * - 在 Wi-Fi 连接成功后调用 doubao_tts_init(play_pcm, NULL) 注册；
 * - doubao_tts 内部每解码出一小段 PCM，就会调用本回调交给 I2S 或其他播放模块。
 *
 * @param samples PCM16 单声道样本数组，只在回调期间有效。
 * @param sample_count 样本数量，不是字节数。
 * @param sample_rate_hz 当前音频采样率，播放端应按此采样率输出。
 * @param user_ctx doubao_tts_init() 传入的用户上下文指针，可为 NULL。
 */
typedef void (*doubao_tts_pcm_output_cb_t)(const int16_t *samples,
                                           size_t sample_count,
                                           int sample_rate_hz,
                                           void *user_ctx);

/**
 * @brief 初始化 Doubao-TTS 2.0 播放模块。
 *
 * 调用方法：
 * 1. 准备一个 PCM 播放回调，把 PCM 写入你的 I2S Speaker；
 * 2. 在 Wi-Fi 连接成功后调用 doubao_tts_init(callback, user_ctx)；
 * 3. 返回 ESP_OK 后即可调用 doubao_tts_play_text("你好")。
 *
 * @param pcm_output 解码后 PCM 输出回调，不能为 NULL。
 * @param user_ctx 传给 pcm_output 的用户上下文指针，可为 NULL。
 * @return ESP_OK 表示初始化成功；ESP_ERR_INVALID_ARG 表示回调为空；ESP_ERR_INVALID_STATE 表示 API Key 未配置。
 */
esp_err_t doubao_tts_init(doubao_tts_pcm_output_cb_t pcm_output, void *user_ctx);

/**
 * @brief 同步播放一段文本。
 *
 * 调用方法：
 * 1. 确保 Wi-Fi 已连接并可访问互联网；
 * 2. 确保已调用 doubao_tts_init()；
 * 3. 调用 doubao_tts_play_text(MAIN_TTS_TEST_TEXT)；
 * 4. 函数会阻塞直到 HTTP 下载、MP3 解码和 PCM 播放流程结束。
 *
 * @param text UTF-8 文本，不能为 NULL 或空字符串，长度不能超过 DOUBAO_TTS_TEXT_MAX_LEN。
 * @return ESP_OK 表示播放流程成功；其他 esp_err_t 表示请求、解析、解码或播放链路失败。
 */
esp_err_t doubao_tts_play_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_TTS_H */
