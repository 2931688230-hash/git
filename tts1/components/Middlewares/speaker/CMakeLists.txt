idf_component_register(SRCS "speaker_task.c"
                            "speaker_player.c"
                            "speaker_resample.c"
                            "speaker_test_pcm.c"
                            "speaker_llm_bridge.c"
                       INCLUDE_DIRS "."
                       REQUIRES IIS app_config
                       PRIV_REQUIRES Middlewares esp_timer freertos log heap esp_system esp_ringbuf)

component_compile_options(-Wno-format)
