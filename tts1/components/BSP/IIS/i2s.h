#ifndef BSP_I2S_H
#define BSP_I2S_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_types.h"

/*
 * I2S 标准模式输出配置区。
 *
 * 说明：
 * - 当前 Doubao-TTS 播放通过回调写入本模块的 I2S PCM 输出接口；
 * - 后续如果换 I2S 功放或改 GPIO，只改本头文件中的宏。
 */

/* I2S BCLK 位时钟引脚。 */
#ifndef BSP_I2S_GPIO_BCLK
#define BSP_I2S_GPIO_BCLK GPIO_NUM_8
#endif

/* I2S WS/LRCK 声道同步引脚。 */
#ifndef BSP_I2S_GPIO_WS
#define BSP_I2S_GPIO_WS GPIO_NUM_7
#endif

/* I2S DOUT 音频数据输出引脚。 */
#ifndef BSP_I2S_GPIO_DOUT
#define BSP_I2S_GPIO_DOUT GPIO_NUM_9
#endif

/* 功放使能引脚；输出高电平时打开功放。 */
#ifndef BSP_I2S_GPIO_PA_CTL
#define BSP_I2S_GPIO_PA_CTL GPIO_NUM_1
#endif

/* 使用的 I2S 外设端口号。 */
#ifndef BSP_I2S_PORT
#define BSP_I2S_PORT I2S_NUM_0
#endif

/* I2S 输出采样率，单位 Hz。 */
#ifndef BSP_I2S_SAMPLE_RATE_HZ
#define BSP_I2S_SAMPLE_RATE_HZ 16000
#endif

/* I2S DMA 描述符数量。 */
#ifndef BSP_I2S_DMA_DESC_NUM
#define BSP_I2S_DMA_DESC_NUM 8
#endif

/* 每个 DMA 描述符承载的帧数量。 */
#ifndef BSP_I2S_DMA_FRAME_NUM
#define BSP_I2S_DMA_FRAME_NUM 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S 标准模式输出。
 *
 * 调用方法：
 * - 使用 i2s_play() 或 i2s_play_pcm16_mono() 前可主动调用；
 * - 如果未主动调用，播放函数会在首次播放前自动初始化。
 */
void i2s_init(void);

/**
 * @brief 按字节数阻塞写入一段 I2S PCM 数据。
 *
 * 调用方法：
 * - data 指向 PCM 字节流；
 * - len 是字节数，不是样本数；
 * - 函数会阻塞直到本段数据全部写入 I2S。
 *
 * @param data PCM 数据指针。
 * @param len PCM 数据字节数。
 */
void i2s_play(const int16_t *data, uint32_t len);

/**
 * @brief 阻塞播放一段 PCM16 单声道样本。
 *
 * 调用方法：
 * - samples 指向 int16_t 单声道样本；
 * - sample_count 是样本数量；
 * - 本函数会自动换算成字节数写入 I2S。
 *
 * @param samples PCM16 单声道样本数组。
 * @param sample_count 样本数量。
 */
void i2s_play_pcm16_mono(const int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // BSP_I2S_H
