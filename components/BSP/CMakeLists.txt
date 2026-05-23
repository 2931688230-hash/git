set(src_dirs
            SPI
            IIS
            tts
            wifi
            http_client
            )
set(include_dirs
            SPI
            IIS
            tts
            wifi
            http_client)
set(requires
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
            json)
idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-format)
