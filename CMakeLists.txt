cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32c5)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_websocket_client"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/main"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/app_config"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/BSP"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/BSP/IIS"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/Middlewares"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/Middlewares/speaker")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(project-name)
