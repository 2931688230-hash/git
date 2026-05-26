set(src_dirs
            IIS
            wifi
            )

set(include_dirs
            IIS
            wifi)

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
            nvs_flash)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${public_requires}
                       PRIV_REQUIRES ${private_requires})

component_compile_options(-ffast-math -O3 -Wno-format)
