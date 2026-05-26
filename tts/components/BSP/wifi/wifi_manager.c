#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include "wifi_credentials.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

static bool wifi_connected_reported = false;
static EventGroupHandle_t s_wifi_event_group = NULL;
static TimerHandle_t s_wifi_reconnect_timer = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static const int MAX_RETRY = 5;
static uint8_t s_last_disconnect_reason = 0;

static const char *wifi_reason_to_string(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static const char *wifi_authmode_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2_WPA3_PSK";
    default:
        return "UNKNOWN";
    }
}

static uint32_t wifi_retry_delay_ms(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return 1200;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return 600;
    default:
        return 300;
    }
}

static esp_err_t wifi_scan_target_ap(const char *target_ssid)
{
    wifi_scan_config_t scan_config = {
        .ssid = (uint8_t *)target_ssid,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 200,
    };

    ESP_LOGI(TAG, "Scanning target SSID: %s", target_ssid);

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Target scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    if (ap_count == 0) {
        ESP_LOGW(TAG, "Target SSID not found: %s", target_ssid);
        printf("{\"wifi_status\":\"target_not_found\",\"ssid\":\"%s\"}\n", target_ssid);
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for target AP list");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (ret != ESP_OK) {
        free(ap_list);
        ESP_LOGE(TAG, "Get target AP records failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_ap_record_t best_ap = ap_list[0];
    for (uint16_t i = 1; i < ap_count; i++) {
        if (ap_list[i].rssi > best_ap.rssi) {
            best_ap = ap_list[i];
        }
    }

    free(ap_list);

    ESP_LOGI(TAG, "Target found: ssid=%s, rssi=%d, channel=%u, auth=%s",
             target_ssid, best_ap.rssi, best_ap.primary, wifi_authmode_to_string(best_ap.authmode));
    printf("{\"wifi_status\":\"target_found\",\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"auth\":\"%s\"}\n",
           target_ssid,
           best_ap.rssi,
           best_ap.primary,
           wifi_authmode_to_string(best_ap.authmode));

    return ESP_OK;
}

static void wifi_reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (wifi_is_connected() || s_retry_num >= MAX_RETRY) {
        return;
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        return;
    }

    s_retry_num++;
    ESP_LOGI(TAG, "Retry to connect to the AP, attempt %d", s_retry_num);
    printf("{\"wifi_status\":\"retrying\",\"attempt\":%d,\"reason\":%u,\"delay_ms\":%lu}\n",
           s_retry_num,
           s_last_disconnect_reason,
           (unsigned long)wifi_retry_delay_ms(s_last_disconnect_reason));
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        uint32_t delay_ms = wifi_retry_delay_ms(event->reason);

        s_last_disconnect_reason = event->reason;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u (%s), rssi=%d",
                 event->reason, wifi_reason_to_string(event->reason), event->rssi);

        if (s_retry_num < MAX_RETRY) {
            printf("{\"wifi_status\":\"retry_scheduled\",\"attempt\":%d,\"reason\":%u,\"reason_text\":\"%s\",\"delay_ms\":%lu}\n",
                   s_retry_num + 1,
                   event->reason,
                   wifi_reason_to_string(event->reason),
                   (unsigned long)delay_ms);
            xTimerChangePeriod(s_wifi_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "Failed to connect to the AP");
            printf("{\"wifi_status\":\"failed\",\"reason\":%u,\"reason_text\":\"%s\"}\n",
                   event->reason, wifi_reason_to_string(event->reason));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_retry_num = 0;
        xTimerStop(s_wifi_reconnect_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        printf("{\"wifi_status\":\"connected\",\"ip\":\"" IPSTR "\",\"gateway\":\"" IPSTR "\",\"netmask\":\"" IPSTR "\"}\n",
               IP2STR(&event->ip_info.ip),
               IP2STR(&event->ip_info.gw),
               IP2STR(&event->ip_info.netmask));
    }
}

static void wifi_status_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        if (wifi_is_connected()) {
            if (!wifi_connected_reported) {
                wifi_ap_record_t ap_info;
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
                    netif != NULL &&
                    esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    printf("Wi-Fi connected successfully! SSID: %s, IP: " IPSTR "\n",
                           ap_info.ssid, IP2STR(&ip_info.ip));
                    wifi_connected_reported = true;
                }
            }
        } else if (wifi_connected_reported) {
            wifi_connected_reported = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
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
        return ESP_ERR_NO_MEM;
    }

    s_wifi_reconnect_timer = xTimerCreate("wifi_reconnect",
                                          pdMS_TO_TICKS(1000),
                                          pdFALSE,
                                          NULL,
                                          wifi_reconnect_timer_cb);
    if (s_wifi_reconnect_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xTaskCreate(wifi_status_monitor_task, "wifi_status_monitor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_scan_start(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi scan...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %u access points", ap_count);

    if (ap_count == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP list");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    printf("{\"wifi_ssids\":[");
    for (uint16_t i = 0; i < ap_count; i++) {
        printf("\"%s\"", ap_list[i].ssid);
        if (i < ap_count - 1) {
            printf(",");
        }
    }
    printf("]}");

    free(ap_list);

    ESP_LOGI(TAG, "Wi-Fi scan completed, SSID list sent via UART");
    return ESP_OK;
}

esp_err_t wifi_connect_to_ap(void)
{
    if (WIFI_TARGET_SSID[0] == '\0' || WIFI_TARGET_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi credentials are not set");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .failure_retry_cnt = MAX_RETRY,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, WIFI_TARGET_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_TARGET_PASSWORD, sizeof(wifi_config.sta.password));

    s_retry_num = 0;
    s_last_disconnect_reason = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    xTimerStop(s_wifi_reconnect_timer, 0);

    esp_err_t scan_ret = wifi_scan_target_ap(WIFI_TARGET_SSID);
    if (scan_ret != ESP_OK && scan_ret != ESP_ERR_NOT_FOUND) {
        return scan_ret;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to SSID: %s", WIFI_TARGET_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to AP");
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Unexpected event");
    return ESP_FAIL;
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
