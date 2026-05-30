#ifndef MAIN_CONFIG_H
#define MAIN_CONFIG_H

/*
 * 主程序调试配置。
 *
 * 本文件只放 main 模块后续可能经常调整的参数，避免把测试文本、
 * 延时等调试项散落在 main.c 中。
 */

/* TTS 冒烟测试播放前的等待时间，单位 ms；用于让 Wi-Fi/IP 日志先输出完成。 */
#ifndef MAIN_TTS_TEST_DELAY_MS
#define MAIN_TTS_TEST_DELAY_MS 1000
#endif

/* TTS 冒烟测试文本；烧录后连上 Wi-Fi 会播放这句话，用来验证喇叭是否能出声。 */
#ifndef MAIN_TTS_TEST_TEXT
#define MAIN_TTS_TEST_TEXT "你好，ESP32-C5"
#endif

/* app_main 末尾空闲循环的延时，单位 ms；避免主任务空转占满 CPU。 */
#ifndef MAIN_IDLE_DELAY_MS
#define MAIN_IDLE_DELAY_MS 1
#endif

#ifndef MAIN_AUDIO_TASK_STACK_SIZE
#define MAIN_AUDIO_TASK_STACK_SIZE 4096
#endif

#ifndef MAIN_AUDIO_TASK_PRIORITY
#define MAIN_AUDIO_TASK_PRIORITY 5
#endif

#endif // MAIN_CONFIG_H
