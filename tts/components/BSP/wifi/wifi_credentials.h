#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stddef.h>

/**
 * @brief 单个已知 WiFi 的账号信息。
 *
 * 调用方法：
 * 1. 每一行是一组 WiFi 名和密码，格式为 {"WiFi名", "WiFi密码"}；
 * 2. 旧版 tts 工程仍然通过 WIFI_TARGET_SSID 和 WIFI_TARGET_PASSWORD 连接；
 * 3. 下面两个旧宏默认取 WIFI_KNOWN_LIST 的第一项，避免改动旧版 wifi_manager.c。
 */
typedef struct {
    const char *ssid;      // WiFi 名称，必须和扫描到的 SSID 完全一致，大小写敏感。
    const char *password;  // WiFi 密码，本地测试使用；不建议提交到公开仓库。
} known_wifi_t;

static const known_wifi_t WIFI_KNOWN_LIST[] = {
    {"LAPTOP-G5CAQHC0 3630", "hjh123456"},
    {"AAABBB", "0123456789"},
    {"iPhoneWang", "Wlsz060501"},
    {"hong28133", "HJH123456"},
    /* {"EXAMPLE_WIFI_SSID_2", "EXAMPLE_WIFI_PASSWORD_2"}, */
};

static const size_t WIFI_KNOWN_COUNT = sizeof(WIFI_KNOWN_LIST) / sizeof(WIFI_KNOWN_LIST[0]);

/* 旧版 tts 工程的兼容宏：wifi_manager.c 会读取这两个宏来连接第一组 WiFi。 */
#ifndef WIFI_TARGET_SSID
#define WIFI_TARGET_SSID WIFI_KNOWN_LIST[0].ssid
#endif

#ifndef WIFI_TARGET_PASSWORD
#define WIFI_TARGET_PASSWORD WIFI_KNOWN_LIST[0].password
#endif

#endif // WIFI_CREDENTIALS_H
