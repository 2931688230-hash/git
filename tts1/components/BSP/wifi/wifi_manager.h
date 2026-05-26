#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * WiFi 管理模块调试参数。
 *
 * 调用方法：
 * - 如果附近 WiFi 扫描较慢，可以调大 WIFI_RESCAN_DELAY_MS；
 * - 如果路由器分配 IP 较慢，可以调大 WIFI_CONNECT_TIMEOUT_MS；
 * - 如果后续日志提示重连任务栈不足，再调大 WIFI_RECONNECT_TASK_STACK。
 */
#define WIFI_RESCAN_DELAY_MS          3000
#define WIFI_CONNECT_TIMEOUT_MS       15000
#define WIFI_RECONNECT_TASK_STACK     4096
#define WIFI_RECONNECT_TASK_PRIORITY  5

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi 管理模块。
 *
 * 调用方法：
 * 1. 在 app_main() 中先调用 wifi_manager_init()；
 * 2. 返回 ESP_OK 后，再调用 wifi_connect_to_ap()；
 * 3. 本函数会初始化 NVS、TCP/IP、默认事件循环、STA 网卡和 WiFi 驱动。
 *
 * @return ESP_OK 表示初始化成功；其他 esp_err_t 表示初始化失败。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 扫描并连接到 WIFI_KNOWN_LIST 中信号最强的 WiFi。
 *
 * 调用方法：
 * 1. 先在 wifi_credentials.h 的 WIFI_KNOWN_LIST 中填写 WiFi 名和密码；
 * 2. 调用 wifi_manager_init()；
 * 3. 调用 wifi_connect_to_ap()，本函数会阻塞等待首次连接成功；
 * 4. 运行过程中断线后，后台重连任务会自动重新扫描并连接。
 *
 * @return ESP_OK 表示首次连接成功；ESP_ERR_INVALID_STATE 表示尚未初始化；
 *         ESP_ERR_NO_MEM 表示重连任务创建失败。
 */
esp_err_t wifi_connect_to_ap(void);

/**
 * @brief 查询当前 WiFi 是否已经连接并获取到 IP。
 *
 * 调用方法：
 * - 需要判断联网状态时调用；
 * - 返回 true 表示已经收到 IP_EVENT_STA_GOT_IP。
 *
 * @return 已连接返回 true，未连接返回 false。
 */
bool wifi_is_connected(void);

/**
 * @brief 读取当前已连接 WiFi 的 SSID。
 *
 * 调用方法：
 * - wifi_connect_to_ap() 返回 ESP_OK 后调用；
 * - ssid 缓冲区建议至少 33 字节。
 *
 * @param ssid 输出缓冲区，用于保存 WiFi 名称。
 * @param ssid_len 输出缓冲区长度。
 * @return 读取成功返回 true，未连接或参数无效返回 false。
 */
bool wifi_get_connected_ssid(char *ssid, size_t ssid_len);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
