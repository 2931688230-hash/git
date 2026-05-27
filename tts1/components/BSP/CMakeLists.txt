set(srcs
            IIS/i2s.c
            wifi/wifi_manager.c
            doubao_tts/doubao_tts.c
            doubao_tts/mp3_decoder.c)

set(include_dirs
            IIS
            wifi
            doubao_tts)

set(private_include_dirs
            doubao_tts/third_party)

# Public dependencies exposed by BSP headers.
set(public_requires
            esp_common
            driver)

# Private dependencies used only by BSP implementation files.
set(private_requires
            log
            freertos
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            esp_http_client
            esp-tls
            mbedtls
            json
            esp_ringbuf)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_INCLUDE_DIRS ${private_include_dirs}
                       REQUIRES ${public_requires}
                       PRIV_REQUIRES ${private_requires})

component_compile_options(-ffast-math -O3 -Wno-format)
