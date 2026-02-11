# embed_binary.cmake
# Converts a binary file into a C byte array header.
#
# Usage:
#   cmake -DINPUT_FILE=input.bin -DOUTPUT_FILE=output.h
#         -DARRAY_NAME=data -DSIZE_NAME=data_size -P embed_binary.cmake

if(NOT INPUT_FILE OR NOT OUTPUT_FILE OR NOT ARRAY_NAME OR NOT SIZE_NAME)
    message(FATAL_ERROR "Usage: cmake -DINPUT_FILE=... -DOUTPUT_FILE=... -DARRAY_NAME=... -DSIZE_NAME=... -P embed_binary.cmake")
endif()

# Read binary file as hex
file(READ "${INPUT_FILE}" FILE_CONTENT HEX)
string(LENGTH "${FILE_CONTENT}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

# Use basename only to avoid leaking the full build path into the binary
get_filename_component(INPUT_BASENAME "${INPUT_FILE}" NAME)

# Build the C array line by line
set(ARRAY_CONTENT "/* Auto-generated from ${INPUT_BASENAME} -- do not edit */\n")
string(APPEND ARRAY_CONTENT "#include <stddef.h>\n\n")
string(APPEND ARRAY_CONTENT "const unsigned char ${ARRAY_NAME}[] = {\n")

set(LINE "    ")
set(COUNT 0)
set(POS 0)
while(POS LESS HEX_LEN)
    string(SUBSTRING "${FILE_CONTENT}" ${POS} 2 BYTE_HEX)
    math(EXPR POS "${POS} + 2")
    math(EXPR COUNT "${COUNT} + 1")

    if(POS LESS HEX_LEN)
        string(APPEND LINE "0x${BYTE_HEX}, ")
    else()
        string(APPEND LINE "0x${BYTE_HEX}")
    endif()

    # Line break every 12 bytes for readability
    math(EXPR MOD "${COUNT} % 12")
    if(MOD EQUAL 0 AND POS LESS HEX_LEN)
        string(APPEND ARRAY_CONTENT "${LINE}\n")
        set(LINE "    ")
    endif()
endwhile()

string(APPEND ARRAY_CONTENT "${LINE}\n};\n\n")
string(APPEND ARRAY_CONTENT "const unsigned long ${SIZE_NAME} = ${BYTE_COUNT};\n")

file(WRITE "${OUTPUT_FILE}" "${ARRAY_CONTENT}")
