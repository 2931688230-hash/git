idf_component_register(SRCS "audio_task.c" "pcm_data.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES audio_player
                       PRIV_REQUIRES freertos log)
