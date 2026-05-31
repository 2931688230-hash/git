#ifndef APP_MAIN_CONFIG_H
#define APP_MAIN_CONFIG_H

/*
 * Main program debug/runtime configuration.
 *
 * Keep frequently tuned app_main and speaker test task parameters here instead
 * of scattering them through main.c.
 */

#ifndef MAIN_TTS_TEST_DELAY_MS
#define MAIN_TTS_TEST_DELAY_MS 1000
#endif

#ifndef MAIN_TTS_TEST_TEXT
#define MAIN_TTS_TEST_TEXT "你好，ESP32-C5"
#endif

#ifndef MAIN_IDLE_DELAY_MS
#define MAIN_IDLE_DELAY_MS 1
#endif

#ifndef MAIN_AUDIO_TASK_STACK_SIZE
#define MAIN_AUDIO_TASK_STACK_SIZE 4096
#endif

#ifndef MAIN_AUDIO_TASK_PRIORITY
#define MAIN_AUDIO_TASK_PRIORITY 5
#endif

#endif /* APP_MAIN_CONFIG_H */
