cmake_minimum_required(VERSION 3.16)

# wifi_manager is a sibling component — add its parent directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basic_provisioning)
