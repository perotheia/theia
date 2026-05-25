cmake_minimum_required(VERSION 3.16)
project(supervisor CXX)

# User requested C++14 explicitly.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

# nlohmann/json — header-only, vendored as a single header under
# third_party/. Picked over yaml-cpp in #380: manifests are now
# JSON-canonical and we don't want a yaml-cpp link step.
set(NLOHMANN_JSON_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nlohmann_json"
    CACHE PATH "Path containing nlohmann/json.hpp")

# etcd-cpp-apiv3 (vendored via repo manifest, built into
# third_party/etcd-cpp-apiv3/install/). Wires the supervisor's state
# publisher; see src/etcd_publisher.cpp.
set(ETCD_CPP_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/etcd-cpp-apiv3/install"
    CACHE PATH "etcd-cpp-apiv3 install prefix")
find_package(etcd-cpp-api REQUIRED CONFIG
    HINTS "${ETCD_CPP_PREFIX}/lib/cmake/etcd-cpp-api")

# .proto files are generated from platform/supervisor/system/package.art
# by `artheia gen-proto`, then compiled with protoc into
# build/generated/{ChildState.pb.cc, …}.
#
# We auto-run gen-proto at configure time if the outputs are missing
# or older than the .art source. Requires `artheia` on PATH — install
# with `pip install -e artheia/`. Skip with -DSKIP_GEN_PROTO=ON if you
# want to manage the regeneration manually.
find_program(PROTOC_EXECUTABLE protoc REQUIRED)

set(PROTO_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/generated/proto)
set(PROTO_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
file(MAKE_DIRECTORY ${PROTO_SRC_DIR})

set(ART_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/system/package.art)
if(NOT SKIP_GEN_PROTO)
    # Look on PATH, then in the artheia venv at the repo root.
    find_program(ARTHEIA_EXECUTABLE
        NAMES artheia
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../artheia/.venv/bin
    )
    if(ARTHEIA_EXECUTABLE)
        message(STATUS "Running ${ARTHEIA_EXECUTABLE} gen-proto on ${ART_SOURCE}")
        execute_process(
            COMMAND ${ARTHEIA_EXECUTABLE} gen-proto ${ART_SOURCE} --out ${PROTO_SRC_DIR}
            RESULT_VARIABLE GEN_PROTO_RC
        )
        if(NOT GEN_PROTO_RC EQUAL 0)
            message(WARNING "artheia gen-proto failed (rc=${GEN_PROTO_RC}); "
                            "build will fail if generated/proto/ is empty")
        endif()
    else()
        message(WARNING "artheia not found on PATH — generated/proto/ will not "
                        "be regenerated. Install with `pip install -e artheia/`.")
    endif()
endif()

file(GLOB PROTO_FILES ${PROTO_SRC_DIR}/*.proto)
set(PROTO_GEN_SRCS)
foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PB_CC ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(PB_H  ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h)
    add_custom_command(
        OUTPUT  ${PB_CC} ${PB_H}
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${PROTO_SRC_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "protoc ${PROTO_NAME}.proto"
        VERBATIM
    )
    list(APPEND PROTO_GEN_SRCS ${PB_CC})
endforeach()

add_library(supervisor_proto OBJECT ${PROTO_GEN_SRCS})
target_include_directories(supervisor_proto PUBLIC ${PROTO_GEN_DIR})
target_compile_options(supervisor_proto PRIVATE
    ${PROTOBUF_CFLAGS_OTHER}
    -Wno-unused-parameter      # protoc-generated code
    -Wno-pedantic
)
target_link_libraries(supervisor_proto PUBLIC ${PROTOBUF_LIBRARIES})

add_executable(supervisor
    src/main.cpp
    src/runtime.cpp
    src/spec.cpp
    src/tipc_publisher.cpp
    src/etcd_publisher.cpp
    $<TARGET_OBJECTS:supervisor_proto>
)

target_include_directories(supervisor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
    # etcd-cpp-api's exported targets don't carry
    # INTERFACE_INCLUDE_DIRECTORIES (upstream quirk), so feed the
    # install prefix's include/ manually. Same workaround as
    # supervisor-gui's CMakeLists.txt.
    ${ETCD_CPP_PREFIX}/include
)

target_link_libraries(supervisor PRIVATE
    ${PROTOBUF_LIBRARIES}
    etcd-cpp-api
    pthread
)

# libtombstone — in-process crash forensics, linked by any daemon that
# wants Android-style tombstones written on fatal signals.
add_library(tombstone STATIC
    tombstone/src/tombstone.cpp
)

target_include_directories(tombstone PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tombstone/include
)

# A minimal demo daemon: links libtombstone, takes one of {segv, abort,
# div0} on argv[1] after a delay, so the supervisor demo can show a
# crash with tombstone collection end-to-end.
add_executable(crasher
    tombstone/demo/crasher.cpp
)
target_link_libraries(crasher PRIVATE tombstone)

install(TARGETS supervisor RUNTIME DESTINATION bin)
install(TARGETS crasher RUNTIME DESTINATION bin)
install(TARGETS tombstone ARCHIVE DESTINATION lib)


# ---------------------------------------------------------------------------
# #364 — hermetic snapshot-synthesis test.
#
# Builds a small in-memory supervision tree, runs the same node_sup
# synthesis logic emit_snapshot() does, and asserts the row shape.
# No yaml-cpp, no etcd, no live runtime — just spec.h + the
# supervisor_proto OBJECT lib for ChildState/TreeSnapshot.
# ---------------------------------------------------------------------------
add_executable(test_node_sup_synth
    test/test_node_sup_synth.cpp
    $<TARGET_OBJECTS:supervisor_proto>
)
target_include_directories(test_node_sup_synth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOBUF_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
)
target_link_libraries(test_node_sup_synth PRIVATE
    ${PROTOBUF_LIBRARIES}
    pthread
)


# #361 — Supervisor's trace-config store (data shape only). Pure
# function, no protobuf/yaml/etcd needed.
add_executable(test_trace_config_store
    test/test_trace_config_store.cpp
)
