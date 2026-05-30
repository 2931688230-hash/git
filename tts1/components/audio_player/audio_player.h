#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_types.h"
#include "esp_err.h"

/*
 * ESP32-C5 PDM speaker output.
 *
 * Public input format:
 *   PCM16 mono at 24 kHz
 *
 * Hardware path:
 *   PCM16 mono -> I2S PDM TX PCM2PDM -> GPIO8 PDM_CLK + GPIO7 PDM_DATA
 *
 * Playback writes are handled only inside audio_player.c:
 *   producer -> ring buffer -> audio_i2s_writer task -> nonblocking DMA write
 */

#ifndef AUDIO_PLAYER_PDM_GPIO_CLK
#define AUDIO_PLAYER_PDM_GPIO_CLK GPIO_NUM_8
#endif

#ifndef AUDIO_PLAYER_PDM_GPIO_DATA
#define AUDIO_PLAYER_PDM_GPIO_DATA GPIO_NUM_7
#endif

#ifndef AUDIO_PLAYER_PDM_GPIO_DATA2
#define AUDIO_PLAYER_PDM_GPIO_DATA2 GPIO_NUM_NC
#endif

#ifndef AUDIO_PLAYER_EXPERIMENT_MODE
#define AUDIO_PLAYER_EXPERIMENT_MODE 0
#endif

#ifndef AUDIO_PLAYER_PDM_CLK_INVERT
#define AUDIO_PLAYER_PDM_CLK_INVERT 0
#endif

#ifndef AUDIO_PLAYER_PDM_UPSAMPLE_FP
#define AUDIO_PLAYER_PDM_UPSAMPLE_FP 960
#endif

#ifndef AUDIO_PLAYER_PDM_UPSAMPLE_FS
#define AUDIO_PLAYER_PDM_UPSAMPLE_FS (AUDIO_PLAYER_SAMPLE_RATE_HZ / 100)
#endif

#ifndef AUDIO_PLAYER_GPIO_PA_CTL
#define AUDIO_PLAYER_GPIO_PA_CTL GPIO_NUM_1
#endif

#ifndef AUDIO_PLAYER_PA_ENABLE_LEVEL
#define AUDIO_PLAYER_PA_ENABLE_LEVEL 1
#endif

#ifndef AUDIO_PLAYER_I2S_PORT
#define AUDIO_PLAYER_I2S_PORT I2S_NUM_0
#endif

#ifndef AUDIO_PLAYER_SAMPLE_RATE_HZ
#define AUDIO_PLAYER_SAMPLE_RATE_HZ 24000
#endif

#ifndef AUDIO_PLAYER_BITS_PER_SAMPLE
#define AUDIO_PLAYER_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#endif

#ifndef AUDIO_PLAYER_PDM_SLOT_MODE
#define AUDIO_PLAYER_PDM_SLOT_MODE I2S_SLOT_MODE_MONO
#endif

#ifndef AUDIO_PLAYER_PDM_SLOT_MASK
#define AUDIO_PLAYER_PDM_SLOT_MASK I2S_PDM_SLOT_LEFT
#endif

#ifndef AUDIO_PLAYER_DMA_DESC_NUM
#define AUDIO_PLAYER_DMA_DESC_NUM 8
#endif

#ifndef AUDIO_PLAYER_DMA_FRAME_NUM
#define AUDIO_PLAYER_DMA_FRAME_NUM 512
#endif

#define AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM \
    ((AUDIO_PLAYER_DMA_DESC_NUM) < 8 ? 8 : (AUDIO_PLAYER_DMA_DESC_NUM))

#define AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM \
    ((AUDIO_PLAYER_DMA_FRAME_NUM) < 512 ? 512 : (AUDIO_PLAYER_DMA_FRAME_NUM))

#ifndef AUDIO_PLAYER_CONVERT_CHUNK_SAMPLES
#define AUDIO_PLAYER_CONVERT_CHUNK_SAMPLES AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_player_init(void);

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples);

esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
