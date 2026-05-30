idf_component_register(SRCS "audio_async_log.c" "audio_player.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_common esp_driver_gpio esp_driver_i2s
                       PRIV_REQUIRES audio esp_timer freertos log heap esp_system esp_ringbuf)
