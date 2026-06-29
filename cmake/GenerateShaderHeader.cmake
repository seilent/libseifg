file(GLOB SPV_FILES "${SPV_DIR}/*.spv")

set(HEADER_CONTENT "#pragma once\n#include <cstdint>\n#include <cstddef>\n\nnamespace seifg::shaders {\n\n")

foreach(SPV_FILE ${SPV_FILES})
    get_filename_component(NAME ${SPV_FILE} NAME_WE)
    file(READ ${SPV_FILE} SPV_HEX HEX)
    string(LENGTH "${SPV_HEX}" HEX_LEN)
    math(EXPR BYTE_LEN "${HEX_LEN} / 2")

    set(ARRAY_CONTENT "")
    set(IDX 0)
    while(IDX LESS HEX_LEN)
        string(SUBSTRING "${SPV_HEX}" ${IDX} 2 BYTE)
        if(ARRAY_CONTENT)
            string(APPEND ARRAY_CONTENT ",")
        endif()
        string(APPEND ARRAY_CONTENT "0x${BYTE}")
        math(EXPR IDX "${IDX} + 2")
    endwhile()

    string(APPEND HEADER_CONTENT "inline const uint8_t ${NAME}_spv[] = {${ARRAY_CONTENT}};\n")
    string(APPEND HEADER_CONTENT "inline const size_t ${NAME}_spv_size = ${BYTE_LEN};\n\n")
endforeach()

string(APPEND HEADER_CONTENT "}\n")

file(WRITE "${OUTPUT_HEADER}" "${HEADER_CONTENT}")
