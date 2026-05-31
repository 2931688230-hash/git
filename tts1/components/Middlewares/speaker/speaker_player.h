#ifndef SPEAKER_PLAYER_H
#define SPEAKER_PLAYER_H

#include <stdint.h>

#include "esp_err.h"
#include "iis.h"

/*
 * Speaker PCM playback.
 *
 * Public input format:
 *   PCM16 mono at 24 kHz, or PCM16 mono at 16 kHz via audio_player_play_tts_pcm().
 *
 * Hardware ownership stays in BSP/IIS. This module only consumes PCM buffers,
 * queues fixed-size chunks and asks IIS to write them.
 */

#define AUDIO_PLAYER_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ
#define AUDIO_PLAYER_BITS_PER_SAMPLE IIS_BITS_PER_SAMPLE
#define AUDIO_PLAYER_PDM_SLOT_MODE IIS_PDM_SLOT_MODE
#define AUDIO_PLAYER_PDM_SLOT_MASK IIS_PDM_SLOT_MASK
#define AUDIO_PLAYER_DMA_DESC_NUM IIS_DMA_DESC_NUM
#define AUDIO_PLAYER_DMA_FRAME_NUM IIS_DMA_FRAME_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM IIS_EFFECTIVE_DMA_DESC_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM IIS_EFFECTIVE_DMA_FRAME_NUM

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

#endif /* SPEAKER_PLAYER_H */
