idf_component_register(
    SRCS
        "doubao_tts.c"
        "mp3_decoder.c"
    INCLUDE_DIRS
        "."
    PRIV_INCLUDE_DIRS
        "third_party"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        freertos
        log
        esp_http_client
        esp-tls
        mbedtls
        json
        esp_ringbuf
)

# Keep warning suppression local to the vendored minimp3 based component.
component_compile_options(-Wno-format)
