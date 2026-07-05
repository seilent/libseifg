cmake_minimum_required(VERSION 3.22)
project(seifg-vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SEIFG_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Path to libseifg root")

add_subdirectory(${SEIFG_ENGINE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libseifg-engine EXCLUDE_FROM_ALL)

add_library(seifg-vk SHARED layer.cpp)

set_target_properties(seifg-vk PROPERTIES
    OUTPUT_NAME "seifg-vk"
    PREFIX "lib"
)

target_link_libraries(seifg-vk PRIVATE seifg)

target_compile_definitions(seifg-vk PRIVATE VK_NO_PROTOTYPES)

target_include_directories(seifg-vk PRIVATE
    ${SEIFG_ENGINE_DIR}/include
)
