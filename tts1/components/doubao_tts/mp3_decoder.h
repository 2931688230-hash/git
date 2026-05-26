#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * minimp3 解码包装层配置区。
 *
 * 设计目的：
 * - 本文件只封装 MP3 -> PCM 的解码逻辑，不依赖 HTTP、Doubao、I2S 或 FreeRTOS task；
 * - 调用者负责把网络流拼成字节流并持续喂给 mp3_decoder_decode()；
 * - 解码出的 PCM 通过回调输出，便于移植到不同播放后端。
 */

/* 解码器内部缓存 MP3 输入数据的最大字节数；需要大于单帧 MP3 数据长度。 */
#ifndef MP3_DECODER_INPUT_BUF_SIZE
#define MP3_DECODER_INPUT_BUF_SIZE 8192
#endif

/* 解码器一次输出给回调的最大单声道样本数；越小延迟越低，回调次数越多。 */
#ifndef MP3_DECODER_PCM_CHUNK_SAMPLES
#define MP3_DECODER_PCM_CHUNK_SAMPLES 1152
#endif

/* 支持的最大声道数；MP3 常见为 1 或 2，本模块会把双声道自动混成单声道。 */
#ifndef MP3_DECODER_MAX_CHANNELS
#define MP3_DECODER_MAX_CHANNELS 2
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCM 输出回调类型。
 *
 * 调用方法：
 * - mp3_decoder_decode() 每成功解出一段 PCM 后会调用本回调；
 * - samples 是 PCM16 单声道样本，调用者可以直接写入 I2S；
 * - sample_rate_hz 来自 MP3 帧头，播放端需要按这个采样率输出。
 *
 * @param samples PCM16 单声道样本数组，只在回调期间有效。
 * @param sample_count 样本数量，不是字节数。
 * @param sample_rate_hz MP3 帧头给出的采样率。
 * @param user_ctx 初始化时传入的用户上下文指针。
 * @return ESP_OK 表示回调处理成功；返回错误会中断解码流程。
 */
typedef esp_err_t (*mp3_decoder_pcm_cb_t)(const int16_t *samples,
                                          size_t sample_count,
                                          int sample_rate_hz,
                                          void *user_ctx);

typedef struct mp3_decoder mp3_decoder_t;

/**
 * @brief 创建 MP3 解码器实例。
 *
 * 调用方法：
 * 1. 调用 mp3_decoder_create(pcm_cb, user_ctx) 创建实例；
 * 2. 循环调用 mp3_decoder_decode() 喂入 MP3 流；
 * 3. 流结束后调用 mp3_decoder_flush()；
 * 4. 最后调用 mp3_decoder_destroy() 释放实例。
 *
 * @param pcm_cb PCM 输出回调，不能为 NULL。
 * @param user_ctx 传给 pcm_cb 的用户上下文指针，可为 NULL。
 * @return 成功返回解码器指针，失败返回 NULL。
 */
mp3_decoder_t *mp3_decoder_create(mp3_decoder_pcm_cb_t pcm_cb, void *user_ctx);

/**
 * @brief 销毁 MP3 解码器实例。
 *
 * 调用方法：
 * - 播放完成或发生不可恢复错误后调用；
 * - decoder 可以为 NULL，为 NULL 时函数直接返回。
 *
 * @param decoder mp3_decoder_create() 返回的解码器实例。
 */
void mp3_decoder_destroy(mp3_decoder_t *decoder);

/**
 * @brief 向解码器喂入一段 MP3 字节流并尽可能解码。
 *
 * 调用方法：
 * - 网络或 ringbuffer 每收到一段 MP3 数据，就调用本函数；
 * - 本函数会缓存不完整帧，等后续数据补齐后继续解码；
 * - 如果 PCM 回调返回错误，本函数会把错误返回给调用者。
 *
 * @param decoder 解码器实例。
 * @param data MP3 数据指针。
 * @param len MP3 数据长度，单位 byte。
 * @return ESP_OK 表示处理成功；其他值表示参数、缓存或回调错误。
 */
esp_err_t mp3_decoder_decode(mp3_decoder_t *decoder, const uint8_t *data, size_t len);

/**
 * @brief 通知解码器 MP3 流已经结束。
 *
 * 调用方法：
 * - HTTP 下载结束并且 ringbuffer 中没有更多 MP3 字节后调用；
 * - 如果内部仍残留无法组成完整帧的数据，会打印日志并丢弃。
 *
 * @param decoder 解码器实例。
 * @return ESP_OK 表示结束处理成功。
 */
esp_err_t mp3_decoder_flush(mp3_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif /* MP3_DECODER_H */
