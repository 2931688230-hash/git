#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t http_client_init(void);

esp_err_t http_post(const char *url,
                    const char *post_data,
                    size_t data_len,
                    char *response_buffer,
                    size_t buffer_size,
                    size_t *response_len);

esp_err_t http_post_with_headers(const char *url,
                                 const char *post_data,
                                 size_t data_len,
                                 const char *header_name,
                                 const char *header_value,
                                 char *response_buffer,
                                 size_t buffer_size,
                                 size_t *response_len);

void http_set_timeout(int timeout_ms);
