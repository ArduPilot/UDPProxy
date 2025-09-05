cmake_minimum_required(VERSION 3.23)
project(udpproxy LANGUAGES CXX)

# Executable
add_executable(udpproxy)

target_sources(udpproxy
    PRIVATE
        udpproxy.cpp
        mavlink.cpp
        util.cpp
        keydb.cpp
        websocket.cpp
    PUBLIC
        mavlink.h
        util.h
        keydb.h
        websocket.h
)

target_compile_features(udpproxy PRIVATE cxx_std_17)

# Compiler options
set(COMMON_WARNINGS
)

target_compile_options(udpproxy
    PRIVATE
            -O2
    -Wall
    -g
    -Werror
    -Wextra
    -Werror=format
    -Wpointer-arith
    -Wcast-align
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-redundant-decls
    -Wno-unknown-pragmas
    -Wno-trigraphs
    -Werror=shadow
    -Werror=return-type
    -Werror=unused-result
    -Werror=unused-variable
    -Werror=narrowing
    -Werror=attributes
    -Werror=overflow
    -Werror=parentheses
    -Werror=format-extra-args
    -Werror=ignored-qualifiers
    -Werror=undef
)

# Special compile flag just for mavlink.cpp
set_source_files_properties(mavlink.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-stringop-truncation"
)

# Link libraries
target_link_libraries(udpproxy
    PRIVATE
        tdb
        ssl
        crypto
)

# MAVLink headers generation
set(MAVLINK_DIR ${CMAKE_SOURCE_DIR}/libraries/mavlink2/generated)
set(MAVLINK_PROTOCOL_H ${MAVLINK_DIR}/protocol.h)

add_custom_command(
    OUTPUT ${MAVLINK_PROTOCOL_H}
    COMMAND ${CMAKE_SOURCE_DIR}/regen_headers.sh
    DEPENDS ${CMAKE_SOURCE_DIR}/modules/mavlink/message_definitions/v1.0/all.xml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating MAVLink headers"
)

add_custom_target(generate_mavlink_headers DEPENDS ${MAVLINK_PROTOCOL_H})
add_dependencies(udpproxy generate_mavlink_headers)


# ----------------------
# Testing (via CTest)
# ----------------------
include(CTest)
enable_testing()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Test: check binary with `file`
add_test(NAME binary_file_check
    COMMAND file $<TARGET_FILE:udpproxy>
)

# Test: check keydb.py is syntactically valid
add_test(NAME keydb_py_compile
    COMMAND ${Python3_EXECUTABLE} -m py_compile ${CMAKE_SOURCE_DIR}/keydb.py
)
