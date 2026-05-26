idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES BSP doubao_tts
                       PRIV_REQUIRES freertos log)
