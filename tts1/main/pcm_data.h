#pragma once

/*
 * pcm_data.h
 *
 * 本文件由 pcm_to_c.py 自动生成。
 * PCM 格式：signed 16bit little-endian, mono, 24kHz。
 * pcm_data_len 表示 int16_t 采样点数量，不是字节数。
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
