cmake_minimum_required(VERSION 3.16)

# Pull in the wifi_manager component from the parent directory
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/.."          # wifi_manager itself
    "${CMAKE_CURRENT_SOURCE_DIR}/components"  # stubs
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_manager_test_app)
