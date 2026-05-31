#pragma once

/*
 * Built-in speaker test PCM.
 *
 * Generated PCM format: signed 16-bit little-endian, mono, 24 kHz.
 * pcm_data_len is the int16_t sample count, not byte count.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const int16_t pcm_data[];
extern const uint32_t pcm_data_len;

#define PCM_DATA_SAMPLE_COUNT (86976u)
#define PCM_DATA_SIZE_BYTES (173952u)

#ifdef __cplusplus
}
#endif
