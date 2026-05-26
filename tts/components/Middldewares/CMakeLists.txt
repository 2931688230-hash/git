set(src_dirs
            laba)

set(include_dirs
            laba)

set(requires
            BSP
            driver
            esp_driver_sdm
            esp_driver_gptimer
            freertos)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
