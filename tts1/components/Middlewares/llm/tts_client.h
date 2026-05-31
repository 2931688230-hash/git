/* [SPEAKER_PROJECT_CHANGE] */
#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [SPEAKER_PROJECT_CHANGE] */
esp_err_t tts_client_speak_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TTS_CLIENT_H */
