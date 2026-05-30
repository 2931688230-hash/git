#include "resample.h"

#include <limits.h>

size_t audio_resample_16k_to_24k_output_samples(size_t input_samples)
{
    if (input_samples > (SIZE_MAX - 1U) / 3U) {
        return 0;
    }
    return (input_samples * 3U + 1U) / 2U;
}

esp_err_t audio_resample_16k_to_24k_linear(const int16_t *input,
                                           size_t input_samples,
                                           int16_t *output,
                                           size_t output_capacity,
                                           size_t *output_samples)
{
    if (output_samples != NULL) {
        *output_samples = 0;
    }
    if (input == NULL || output == NULL || output_samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input_samples == 0) {
        return ESP_OK;
    }

    const size_t required_samples = audio_resample_16k_to_24k_output_samples(input_samples);
    if (required_samples == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (output_capacity < required_samples) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t out_index = 0; out_index < required_samples; out_index++) {
        const size_t pos_num = out_index * 2U;
        const size_t in_index = pos_num / 3U;
        const size_t frac = pos_num % 3U;
        const int32_t s0 = input[in_index];
        const int32_t s1 = (in_index + 1U < input_samples) ? input[in_index + 1U] : s0;
        int32_t sample = s0;

        if (frac == 1U) {
            sample = (2 * s0 + s1) / 3;
        } else if (frac == 2U) {
            sample = (s0 + 2 * s1) / 3;
        }

        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        output[out_index] = (int16_t)sample;
    }

    *output_samples = required_samples;
    return ESP_OK;
}
