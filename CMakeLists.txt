cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32c5)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/tts1/main"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/audio"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/audio_player"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/BSP"
    "${CMAKE_CURRENT_LIST_DIR}/tts1/components/Middldewares")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(project-name)
