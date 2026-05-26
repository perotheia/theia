cmake_minimum_required(VERSION 3.16)
project(services_com CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(GRPCPP   REQUIRED grpc++)
find_program(PROTOC_EXECUTABLE     protoc           REQUIRED)
find_program(GRPC_CPP_PLUGIN       grpc_cpp_plugin  REQUIRED)

# Reuse the supervisor's protos verbatim — same wire schema.
set(SUP_PROTO_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/supervisor/generated/proto)
set(COM_PROTO_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# Supervisor proto C++ bindings (the .pb.cc/.pb.h for ChildState, ControlRequest,
# TreeSnapshot, … — the wire types com bridges). Two ways to get them:
#
#   * Bazel-driven build (`bazel build //services/com:services-com`): the
#     hybrid (#proto-as-dep). //platform/supervisor:supervisor_pb_cpp runs
#     protoc --cpp_out once over every supervisor .proto and hands us the
#     compiled libsupervisor_pb_cpp.a + headers via the rules_foreign_cc
#     sandbox ($EXT_BUILD_DEPS/{lib,include}). We link the .a and DON'T run
#     protoc on these — set -DTHEIA_SUP_PB_LIB=<path-to-.a> to take this path.
#   * Standalone dev build (`cmake -S . -B build`): no Bazel, no
#     $EXT_BUILD_DEPS — fall back to running protoc here over SUP_PROTO_DIR.
#
# Either way the generated headers are flat (`#include "ChildState.pb.h"`,
# protos import each other with bare relative paths), so a single include
# dir — PROTO_GEN_DIR or $EXT_BUILD_DEPS/include — satisfies both the
# supervisor types AND supervisor_bridge.pb.cc (which #includes them). Keep
# the list in sync with platform/supervisor/BUILD.bazel _SUP_PROTOS.
set(SUP_PROTOS
    ChildSelector ChildSpec ChildState ConfigureLogLevelRequest
    ConfigureTraceRequest ControlReply
    ControlRequest DeleteChildRequest GetChildRequest GetSystemInfoRequest
    GetTraceConfigRequest GetTreeRequest HealthBeacon HeartbeatReport
    LogLevelConfig
    SendTimeoutReport SocketInfo StartChildRequest SupervisionEvent
    SystemInfo ThreadSample TraceConfig TraceConfigList TreeSnapshot)

# Bazel hybrid flips this ON via -D. Then we DON'T run protoc on the
# supervisor protos — we find the prebuilt libsupervisor_pb_cpp.a that
# rules_foreign_cc staged into $EXT_BUILD_DEPS/lib (on CMAKE_PREFIX_PATH).
# A plain bool, so no $EXT_BUILD_DEPS path-interpolation in the -D value.
option(THEIA_USE_BAZEL_SUP_PB
    "Consume Bazel's prebuilt supervisor protos instead of running protoc here" OFF)

set(SUP_PROTO_SRCS)
set(SUP_PB_LIB "")
if(NOT THEIA_USE_BAZEL_SUP_PB)
    # Standalone CMake: generate + compile the supervisor protos ourselves.
    foreach(P ${SUP_PROTOS})
        set(PB_CC ${PROTO_GEN_DIR}/${P}.pb.cc)
        set(PB_H  ${PROTO_GEN_DIR}/${P}.pb.h)
        add_custom_command(
            OUTPUT ${PB_CC} ${PB_H}
            COMMAND ${PROTOC_EXECUTABLE}
                    --proto_path=${SUP_PROTO_DIR}
                    --cpp_out=${PROTO_GEN_DIR}
                    ${SUP_PROTO_DIR}/${P}.proto
            DEPENDS ${SUP_PROTO_DIR}/${P}.proto
            COMMENT "protoc ${P}.proto"
            VERBATIM
        )
        list(APPEND SUP_PROTO_SRCS ${PB_CC})
    endforeach()
else()
    # CMAKE_PREFIX_PATH's first entry is $EXT_BUILD_DEPS (rules_foreign_cc);
    # the staged static lib is at its lib/ subdir. Reference the .a
    # explicitly — both .a and .so are staged, and we want the static one
    # statically linked (no runtime .so dependency in the deployed binary).
    list(GET CMAKE_PREFIX_PATH 0 _EXT_BUILD_DEPS)
    set(SUP_PB_LIB "${_EXT_BUILD_DEPS}/lib/libsupervisor_pb_cpp.a")
    if(NOT EXISTS "${SUP_PB_LIB}")
        message(FATAL_ERROR
            "THEIA_USE_BAZEL_SUP_PB=ON but ${SUP_PB_LIB} not found — the \
//platform/supervisor:supervisor_pb_cpp dep didn't stage into the sandbox.")
    endif()
    message(STATUS "services-com: using prebuilt supervisor protos: ${SUP_PB_LIB}")
endif()

# com's own .proto needs both standard protoc (for the message types)
# and grpc_cpp_plugin (for the service stubs). Reference the supervisor
# protos via -I so imports resolve.
set(BRIDGE_PROTO_PB_CC      ${PROTO_GEN_DIR}/supervisor_bridge.pb.cc)
set(BRIDGE_PROTO_PB_H       ${PROTO_GEN_DIR}/supervisor_bridge.pb.h)
set(BRIDGE_PROTO_GRPC_CC    ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.cc)
set(BRIDGE_PROTO_GRPC_H     ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.h)

add_custom_command(
    OUTPUT  ${BRIDGE_PROTO_PB_CC} ${BRIDGE_PROTO_PB_H}
            ${BRIDGE_PROTO_GRPC_CC} ${BRIDGE_PROTO_GRPC_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${COM_PROTO_DIR}
            --proto_path=${SUP_PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${COM_PROTO_DIR}/supervisor_bridge.proto
    DEPENDS ${COM_PROTO_DIR}/supervisor_bridge.proto
    COMMENT "protoc (+grpc) supervisor_bridge.proto"
    VERBATIM
)

# Robot node (#387) — test-only signal inject + service call. ON for
# dev/test images; flip OFF (or via the services manifest → -D) for a
# release build so the inject/call RPCs and their TIPC client are
# compiled out entirely.
option(THEIA_ROBOT_NODE "Build the test-only robot signal/call node" ON)

set(COM_SRCS
    src/main.cpp
    src/tipc_uplink.cpp
)
if(THEIA_ROBOT_NODE)
    list(APPEND COM_SRCS src/robot_node.cpp)
endif()

add_executable(services-com
    ${COM_SRCS}
    ${SUP_PROTO_SRCS}
    ${BRIDGE_PROTO_PB_CC}
    ${BRIDGE_PROTO_GRPC_CC}
)

if(THEIA_ROBOT_NODE)
    target_compile_definitions(services-com PRIVATE THEIA_ROBOT_NODE)
endif()

target_include_directories(services-com PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
)

# In the Bazel hybrid the supervisor .pb.h headers live in the dep sandbox
# (CMAKE_PREFIX_PATH = $EXT_BUILD_DEPS). supervisor_bridge.pb.cc #includes
# them, so put each prefix's include/ on the path explicitly (find_path
# isn't enough — these aren't a find_package'd lib).
if(THEIA_USE_BAZEL_SUP_PB)
    foreach(P ${CMAKE_PREFIX_PATH})
        if(EXISTS ${P}/include)
            target_include_directories(services-com PRIVATE ${P}/include)
        endif()
    endforeach()
endif()

# Bazel hybrid: link the prebuilt supervisor proto bindings instead of the
# SUP_PROTO_SRCS we'd otherwise compile in-tree. The .a has undefined
# google::protobuf symbols satisfied by -lprotobuf, and supervisor_bridge's
# objects (already in the target) reference the .a — wrap both in a
# --start-group so lld extracts members regardless of order. Must come
# BEFORE the protobuf/grpc libs below in the PRIVATE link order.
if(THEIA_USE_BAZEL_SUP_PB)
    target_link_libraries(services-com PRIVATE
        -Wl,--start-group ${SUP_PB_LIB} ${PROTOBUF_LIBRARIES} -Wl,--end-group
    )
endif()

target_link_libraries(services-com PRIVATE
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    -lgrpc -lgpr
    pthread
)

# Suppress noisy protobuf-generated-source warnings.
target_compile_options(services-com PRIVATE
    -Wno-unused-parameter
    -Wno-pedantic
)

install(TARGETS services-com RUNTIME DESTINATION bin)
