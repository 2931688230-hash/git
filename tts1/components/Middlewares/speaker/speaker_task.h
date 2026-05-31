#ifndef SPEAKER_TASK_H
#define SPEAKER_TASK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the standalone speaker playback task.
 *
 * The function name is kept as audio_task_start() for this migration round to
 * avoid broad call-site churn.
 */
esp_err_t audio_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_TASK_H */
