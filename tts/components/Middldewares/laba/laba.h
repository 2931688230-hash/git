// Simple speaker (laba) middleware for ESP-SensairShuttle (ESP32-C5).
// Output: differential PDM on PDM_P/PDM_N + PA_CTL enable.
// Input PCM format: signed 16-bit, mono.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize speaker output (SDM + esp_timer + PA enable).
void laba_init(void);

// Set software volume in range [0.0f, 1.0f]. Default is 1.0f.
void laba_set_volume(float volume);

// Play PCM16 mono samples (blocking).
void laba_play_pcm16_mono(const int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif
