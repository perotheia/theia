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

# Supervisor protos — only the ones com directly references. Standard
# protoc (no grpc plugin). Same generated_proto layout used elsewhere.
set(SUP_PROTOS
    ChildSelector ChildSpec ChildState ConfigureLogLevelRequest
    ConfigureTraceRequest ControlReply
    ControlRequest DeleteChildRequest GetChildRequest GetSystemInfoRequest
    GetTraceConfigRequest GetTreeRequest HealthBeacon HeartbeatReport
    LogLevelConfig
    SendTimeoutReport SocketInfo StartChildRequest SupervisionEvent
    SystemInfo ThreadSample TraceConfig TraceConfigList TreeSnapshot)
set(SUP_PROTO_SRCS)
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
    src/tipc_trace_uplink.cpp
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
