#include "http_client.h"

#include <stdbool.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "http_client";

static int s_http_timeout_ms = 20000;

typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t data_len;
    bool overflow;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || response == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t copy_len = (size_t)evt->data_len;
    if (response->data_len + copy_len >= response->buffer_size) {
        copy_len = response->buffer_size - response->data_len - 1;
        response->overflow = true;
    }

    if (copy_len > 0) {
        memcpy(response->buffer + response->data_len, evt->data, copy_len);
        response->data_len += copy_len;
        response->buffer[response->data_len] = '\0';
    }

    return response->overflow ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t http_client_init(void)
{
    ESP_LOGI(TAG, "HTTP client ready");
    return ESP_OK;
}

esp_err_t http_post(const char *url,
                    const char *post_data,
                    size_t data_len,
                    char *response_buffer,
                    size_t buffer_size,
                    size_t *response_len)
{
    return http_post_with_headers(url, post_data, data_len, NULL, NULL,
                                  response_buffer, buffer_size, response_len);
}

esp_err_t http_post_with_headers(const char *url,
                                 const char *post_data,
                                 size_t data_len,
                                 const char *header_name,
                                 const char *header_value,
                                 char *response_buffer,
                                 size_t buffer_size,
                                 size_t *response_len)
{
    if (url == NULL || post_data == NULL || response_buffer == NULL ||
        response_len == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buffer[0] = '\0';
    *response_len = 0;

    http_response_t response = {
        .buffer = response_buffer,
        .buffer_size = buffer_size,
        .data_len = 0,
        .overflow = false,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = s_http_timeout_ms,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (header_name != NULL && header_value != NULL) {
        esp_http_client_set_header(client, header_name, header_value);
    }
    esp_http_client_set_post_field(client, post_data, data_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP POST failed, status=%d, body=%s",
                     status_code, response_buffer);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST error: %s", esp_err_to_name(err));
    }

    if (err == ESP_OK && response.overflow) {
        ESP_LOGE(TAG, "HTTP response buffer is too small");
        err = ESP_ERR_NO_MEM;
    }

    *response_len = response.data_len;
    esp_http_client_cleanup(client);
    return err;
}

void http_set_timeout(int timeout_ms)
{
    s_http_timeout_ms = timeout_ms;
}
