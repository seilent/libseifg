cmake_minimum_required(VERSION 3.22)
project(libseifg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET volk)
    include(FetchContent)
    FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG vulkan-sdk-1.3.290.0
    )
    FetchContent_MakeAvailable(volk)
endif()

include(cmake/CompileShaders.cmake)

add_library(seifg STATIC
    src/seifg.cpp
    src/engine.cpp
    src/core/device.cpp
    src/core/image.cpp
    src/core/pipeline.cpp
    src/core/descriptor.cpp
    src/core/command.cpp
    src/core/sampler.cpp
)

target_include_directories(seifg PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_include_directories(seifg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(seifg PUBLIC volk)

if(ANDROID)
    target_link_libraries(seifg PRIVATE android)
endif()

target_compile_definitions(seifg PUBLIC
    VK_NO_PROTOTYPES
)

if(ANDROID)
    target_compile_definitions(seifg PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()

if(SEIFG_SHADER_SOURCES)
    add_dependencies(seifg seifg_shaders)
endif()

if(SEIFG_BUILD_TESTS)
    add_executable(seifg_smoke tests/smoke.cpp)
    target_link_libraries(seifg_smoke PRIVATE seifg volk dl)
    if(ANDROID)
        target_link_libraries(seifg_smoke PRIVATE android log)
    endif()
    if(SEIFG_SHADER_SOURCES)
        add_dependencies(seifg_smoke seifg_shaders)
    endif()
endif()
