#include "wifi_manager.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

static const char *TAG = "wifi_manager";

/* WiFi 事件组：连接成功、需要重连、本轮连接断开分别占一个 bit。 */
static EventGroupHandle_t s_wifi_event_group;
static TaskHandle_t s_wifi_reconnect_task_handle;

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_RECONNECT_BIT = BIT1,
    WIFI_DISCONNECTED_BIT = BIT2,
};

/**
 * @brief 在已知 WiFi 列表中按 SSID 查找账号。
 *
 * 调用方法：
 * - scan_strongest_known_wifi() 扫描到 AP 后调用；
 * - SSID 必须完全一致，大小写敏感。
 *
 * @param ssid 扫描到的 WiFi 名称。
 * @return 找到时返回 WIFI_KNOWN_LIST 中对应项指针，未找到返回 NULL。
 */
static const known_wifi_t *find_known_wifi_by_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < WIFI_KNOWN_COUNT; i++) {
        if (strcmp(ssid, WIFI_KNOWN_LIST[i].ssid) == 0) {
            return &WIFI_KNOWN_LIST[i];
        }
    }

    return NULL;
}

/**
 * @brief 扫描周围 AP，并选择 RSSI 最强的已知 WiFi。
 *
 * 调用方法：
 * - wifi_reconnect_task() 每次准备连接前调用；
 * - 本函数只返回 WIFI_KNOWN_LIST 中存在的 WiFi，不会连接未知热点。
 *
 * @param selected_wifi 输出参数，返回选中的 WiFi 账号指针。
 * @param selected_rssi 输出参数，返回选中 AP 的 RSSI。
 * @return 成功找到已知 WiFi 返回 ESP_OK，否则返回 ESP_ERR_NOT_FOUND 或扫描错误。
 */
static esp_err_t scan_strongest_known_wifi(const known_wifi_t **selected_wifi,
                                           int8_t *selected_rssi)
{
    if (selected_wifi == NULL || selected_rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *selected_wifi = NULL;
    *selected_rssi = INT8_MIN;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 80,
    };

    ESP_LOGI(TAG, "Scanning known WiFi...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get AP count failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Found %u access points", (unsigned int)ap_count);
    if (ap_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (ret != ESP_OK) {
        free(ap_list);
        ESP_LOGE(TAG, "Get AP records failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        const known_wifi_t *known_wifi =
            find_known_wifi_by_ssid((const char *)ap_list[i].ssid);
        if (known_wifi == NULL) {
            continue;
        }

        if (*selected_wifi == NULL || ap_list[i].rssi > *selected_rssi) {
            *selected_wifi = known_wifi;
            *selected_rssi = ap_list[i].rssi;
        }
    }

    free(ap_list);

    if (*selected_wifi == NULL) {
        ESP_LOGW(TAG, "No known WiFi found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "Selected WiFi: %s, RSSI: %d",
             (*selected_wifi)->ssid,
             *selected_rssi);
    return ESP_OK;
}

/**
 * @brief 根据选中的已知 WiFi 配置 STA 并发起连接。
 *
 * 调用方法：
 * - wifi_reconnect_task() 扫描到可用账号后调用；
 * - 本函数只发起连接，最终成功与否由 WiFi/IP 事件回调确认。
 *
 * @param selected_wifi 扫描后选中的已知 WiFi 账号。
 * @param selected_rssi 扫描到的 RSSI，仅用于日志输出。
 * @return 成功发起连接返回 ESP_OK，否则返回 ESP-IDF 错误码。
 */
static esp_err_t connect_selected_wifi(const known_wifi_t *selected_wifi,
                                       int8_t selected_rssi)
{
    if (selected_wifi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_config.sta.ssid,
            selected_wifi->ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            selected_wifi->password,
            sizeof(wifi_config.sta.password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set WiFi config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);

    ESP_LOGI(TAG,
             "Connecting to SSID: %s, RSSI: %d",
             selected_wifi->ssid,
             selected_rssi);

    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "Start WiFi connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief WiFi 后台重连任务。
 *
 * 调用方法：
 * - wifi_connect_to_ap() 首次调用时创建本任务；
 * - 任务被 WIFI_RECONNECT_BIT 唤醒后会扫描已知 WiFi 并尝试连接；
 * - 断线事件发生时，事件回调会再次设置 WIFI_RECONNECT_BIT。
 */
static void wifi_reconnect_task(void *arg)
{
    (void)arg;

    while (true) {
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_RECONNECT_BIT,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        while (!wifi_is_connected()) {
            const known_wifi_t *selected_wifi = NULL;
            int8_t selected_rssi = 0;

            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);

            esp_err_t ret = scan_strongest_known_wifi(&selected_wifi, &selected_rssi);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "No connectable known WiFi yet, rescan in %d ms",
                         WIFI_RESCAN_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_DELAY_MS));
                continue;
            }

            ret = connect_selected_wifi(selected_wifi, selected_rssi);
            if (ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_DELAY_MS));
                continue;
            }

            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
            if (bits & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "WiFi connected");
                break;
            }

            if (bits & WIFI_DISCONNECTED_BIT) {
                ESP_LOGW(TAG,
                         "WiFi connection attempt failed, rescan in %d ms",
                         WIFI_RESCAN_DELAY_MS);
            } else {
                ESP_LOGW(TAG,
                         "WiFi connection timeout, rescan in %d ms",
                         WIFI_RESCAN_DELAY_MS);
                esp_wifi_disconnect();
            }

            vTaskDelay(pdMS_TO_TICKS(WIFI_RESCAN_DELAY_MS));
        }
    }
}

/**
 * @brief WiFi/IP 事件处理函数。
 *
 * 调用方法：
 * - wifi_manager_init() 通过 esp_event_handler_register() 注册；
 * - STA 启动、断线、拿到 IP 时由 ESP-IDF 自动调用。
 */
static void event_handler(void *arg,
                          esp_event_base_t event_base,
                          int32_t event_id,
                          void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        int reason = disconnected != NULL ? disconnected->reason : -1;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d, rescan scheduled", reason);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(s_wifi_event_group, WIFI_RECONNECT_BIT | WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Create WiFi event group failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(void)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_wifi_reconnect_task_handle == NULL) {
        BaseType_t task_created = xTaskCreate(wifi_reconnect_task,
                                              "wifi_reconnect",
                                              WIFI_RECONNECT_TASK_STACK,
                                              NULL,
                                              WIFI_RECONNECT_TASK_PRIORITY,
                                              &s_wifi_reconnect_task_handle);
        if (task_created != pdPASS) {
            s_wifi_reconnect_task_handle = NULL;
            ESP_LOGE(TAG, "Create WiFi reconnect task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_BIT);
    ESP_LOGI(TAG, "Waiting for WiFi connection");

    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_wifi_event_group != NULL &&
           (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_get_connected_ssid(char *ssid, size_t ssid_len)
{
    if (ssid == NULL || ssid_len == 0 || !wifi_is_connected()) {
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    strlcpy(ssid, (const char *)ap_info.ssid, ssid_len);
    return true;
}
