#pragma once

#include "esp_err.h"

/* Manual TTS settings: edit these values in one place. */
#ifndef TTS_READ_BUF_SIZE
#define TTS_READ_BUF_SIZE 1024
#endif

#ifndef WAV_HEADER_MAX_SIZE
#define WAV_HEADER_MAX_SIZE 4096
#endif

#ifndef TTS_HTTP_TIMEOUT_MS
#define TTS_HTTP_TIMEOUT_MS 30000
#endif

#ifndef TTS_API_URL
#define TTS_API_URL "https://api.ruozhen.ggff.net/v1/audio/speech"
#endif

#ifndef TTS_API_KEY
#define TTS_API_KEY ""
#endif

#ifndef TTS_MODEL_NAME
#define TTS_MODEL_NAME "gpt-5.5"
#endif

#ifndef TTS_VOICE_NAME
#define TTS_VOICE_NAME "alloy"
#endif

esp_err_t tts_init(void);
void tts_play_text(const char *text);
