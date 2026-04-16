cmake_minimum_required(VERSION 3.16)
project(canbus-sender LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Standards & build type
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD   17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Speed flags — tune for the RPi's ARM core at compile time when cross-compiling;
# -march=native works for a native build on the Pi itself.
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    -Wall -Wextra -Wpedantic
)

# ---------------------------------------------------------------------------
# Firmware version (override at cmake time: -DFIRMWARE_VERSION=x.y.z)
# ---------------------------------------------------------------------------
if(NOT DEFINED FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "1.0.0")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# --- protobuf-c ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF_C REQUIRED libprotobuf-c)

# --- OpenSSL (for SHA-256) ---
find_package(OpenSSL REQUIRED)

# --- protoc + protoc-gen-c (needed only at build time) ---
find_program(PROTOC       protoc       REQUIRED
    DOC "Google protobuf compiler")
find_program(PROTOC_GEN_C protoc-gen-c REQUIRED
    DOC "protoc-gen-c plugin (from protobuf-c)")

# ---------------------------------------------------------------------------
# Generate protobuf-c C sources from proto/journeyos_device.proto
# ---------------------------------------------------------------------------
set(PROTO_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

set(PROTO_FILE     "${PROTO_SRC_DIR}/journeyos_device.proto")
set(PROTO_C_SRC    "${PROTO_GEN_DIR}/journeyos_device.pb-c.c")
set(PROTO_C_HDR    "${PROTO_GEN_DIR}/journeyos_device.pb-c.h")

add_custom_command(
    OUTPUT  "${PROTO_C_SRC}" "${PROTO_C_HDR}"
    COMMAND "${PROTOC}"
            "--plugin=protoc-gen-c=${PROTOC_GEN_C}"
            "--c_out=${PROTO_GEN_DIR}"
            "-I${PROTO_SRC_DIR}"
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating protobuf-c sources from journeyos_device.proto"
    VERBATIM
)

# Wrap in a custom target so IDEs show the generated files
add_custom_target(proto_gen DEPENDS "${PROTO_C_SRC}" "${PROTO_C_HDR}")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(canbus-sender
    src/main.cpp
    src/config.cpp
    src/dhcp.cpp
    src/proto_io.cpp
    src/can_probe.cpp
    src/bridge.cpp
    "${PROTO_C_SRC}"
)

add_dependencies(canbus-sender proto_gen)

target_include_directories(canbus-sender PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROTO_GEN_DIR}"
    ${PROTOBUF_C_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(canbus-sender PRIVATE
    ${PROTOBUF_C_LIBRARIES}
    OpenSSL::Crypto
)

target_compile_options(canbus-sender PRIVATE
    ${PROTOBUF_C_CFLAGS_OTHER}
)

target_compile_definitions(canbus-sender PRIVATE
    FIRMWARE_VERSION="${FIRMWARE_VERSION}"
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS canbus-sender RUNTIME DESTINATION bin)