#ifndef BSP_I2S_H
#define BSP_I2S_H

#include <stddef.h>
#include <stdint.h>

void i2s_init(void);
void i2s_play(int16_t *data, uint32_t len);
void i2s_play_pcm16_mono(const int16_t *samples, size_t sample_count);

#endif
