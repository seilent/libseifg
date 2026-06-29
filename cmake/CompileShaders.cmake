file(GLOB SEIFG_SHADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp")

if(DEFINED ENV{NDK})
    set(SEIFG_NDK "$ENV{NDK}")
elseif(DEFINED CMAKE_ANDROID_NDK)
    set(SEIFG_NDK "${CMAKE_ANDROID_NDK}")
else()
    set(SEIFG_NDK "$ENV{HOME}/Android/Sdk/ndk/28.2.13676358")
endif()

set(SEIFG_NDK_GLSLANG "${SEIFG_NDK}/shader-tools/linux-x86_64/glslangValidator")

if(EXISTS "${SEIFG_NDK_GLSLANG}")
    set(GLSLANG_VALIDATOR "${SEIFG_NDK_GLSLANG}")
else()
    find_program(GLSLANG_VALIDATOR glslangValidator)
endif()

if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found")
endif()

set(SEIFG_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(SEIFG_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/include/seifg_shaders.h")

file(MAKE_DIRECTORY "${SEIFG_SPV_DIR}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")

set(SEIFG_SPV_FILES "")

foreach(SHADER ${SEIFG_SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
    set(SPV_FILE "${SEIFG_SPV_DIR}/${SHADER_NAME}.spv")
    list(APPEND SEIFG_SPV_FILES ${SPV_FILE})

    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 -o ${SPV_FILE} ${SHADER}
        DEPENDS ${SHADER}
        VERBATIM
    )
endforeach()

if(SEIFG_SHADER_SOURCES)
    add_custom_command(
        OUTPUT ${SEIFG_SHADER_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DSPV_DIR=${SEIFG_SPV_DIR}
            -DOUTPUT_HEADER=${SEIFG_SHADER_HEADER}
            -DSHADER_SOURCES="${SEIFG_SHADER_SOURCES}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateShaderHeader.cmake
        DEPENDS ${SEIFG_SPV_FILES}
        VERBATIM
    )

    add_custom_target(seifg_shaders DEPENDS ${SEIFG_SHADER_HEADER})
endif()
