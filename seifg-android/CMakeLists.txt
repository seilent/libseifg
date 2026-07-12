cmake_minimum_required(VERSION 3.21)
project(seifg-android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(shadowhook
    GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
    GIT_TAG v2.0.1
)
FetchContent_Populate(shadowhook)
add_subdirectory(
    ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp
    ${CMAKE_BINARY_DIR}/shadowhook-build
)

if(TARGET shadowhook)
    target_compile_options(shadowhook PRIVATE -Wno-error -Wno-unsafe-buffer-usage -Wno-declaration-after-statement)
    target_link_options(shadowhook PRIVATE -Wl,--undefined-version)
endif()
if(TARGET shadowhook_nothing)
    target_compile_options(shadowhook_nothing PRIVATE -Wno-error -Wno-unsafe-buffer-usage -Wno-declaration-after-statement)
endif()

set(SEIFG_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SEIFG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SEIFG_ENGINE_DIR} ${CMAKE_BINARY_DIR}/libseifg-engine EXCLUDE_FROM_ALL)

file(GLOB MODULE_SRC src/module.cc src/fpslimiter.cc)
file(GLOB UTILITY_SRC src/utility/*.cc)
file(GLOB FILEWATCH_SRC src/file_watch/*.cc)
file(GLOB DISPATCHER_SRC src/file_watch/dispatcher/*.cc)
file(GLOB JNIHELPER_SRC src/jni_helper/*.cc)
file(GLOB UNITY_SRC src/unity/*.cc)
file(GLOB CAPTURE_SRC src/capture/*.cc)
file(GLOB XDL_SRC third/xdl/*.c)
file(GLOB RIRU_HIDE_SRC third/riru_hide/*.c third/riru_hide/*.cc)

find_library(LOG_LIB log)
find_library(VK_LIB vulkan)
find_library(EGL_LIB EGL)
find_library(GLES_LIB GLESv2)
find_library(GLES3_LIB GLESv3)
find_library(ANDROID_LIB android)

add_library(seifg-android SHARED
    ${MODULE_SRC}
    ${UTILITY_SRC}
    ${FILEWATCH_SRC}
    ${DISPATCHER_SRC}
    ${JNIHELPER_SRC}
    ${UNITY_SRC}
    ${CAPTURE_SRC}
    ${XDL_SRC}
    ${RIRU_HIDE_SRC}
)

target_include_directories(seifg-android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third
    ${CMAKE_CURRENT_SOURCE_DIR}/third/xdl
    ${CMAKE_CURRENT_SOURCE_DIR}/third/rapidjson
    ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp/include
    ${SEIFG_ENGINE_DIR}/include
)

string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UP)
target_compile_options(seifg-android PRIVATE -fno-rtti -fno-exceptions -fvisibility=hidden)
if(NOT BUILD_TYPE_UP STREQUAL "DEBUG")
    target_compile_options(seifg-android PRIVATE -Oz -flto -fdata-sections -ffunction-sections)
    target_link_options(seifg-android PRIVATE -flto -Wl,--gc-sections -Wl,--exclude-libs,ALL -s)
endif()

target_link_libraries(seifg-android PRIVATE ${LOG_LIB} ${VK_LIB} ${EGL_LIB} ${GLES_LIB} ${GLES3_LIB} ${ANDROID_LIB} dl seifg)

set_target_properties(seifg-android PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${ANDROID_ABI}"
)
