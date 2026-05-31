#ifndef SPEAKER_RESAMPLE_H
#define SPEAKER_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_RESAMPLE_16K_HZ 16000
#define AUDIO_RESAMPLE_24K_HZ 24000

size_t audio_resample_16k_to_24k_output_samples(size_t input_samples);

esp_err_t audio_resample_16k_to_24k_linear(const int16_t *input,
                                           size_t input_samples,
                                           int16_t *output,
                                           size_t output_capacity,
                                           size_t *output_samples);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_RESAMPLE_H */
