set(srcs
            SPI/spi.c
            IIS/i2s.c
            wifi/wifi_manager.c
            http_client/http_client.c
            doubao_tts/doubao_tts.c
            doubao_tts/mp3_decoder.c)

set(include_dirs
            SPI
            IIS
            wifi
            http_client
            doubao_tts)

set(private_include_dirs
            doubao_tts/third_party)

set(requires
            esp_common
            driver
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
                       REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-format)
