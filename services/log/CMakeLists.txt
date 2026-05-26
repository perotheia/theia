cmake_minimum_required(VERSION 3.16)
project(services_log CXX)

# services/log[trace] — TraceCollector daemon + its OWN gRPC TraceStream.
#
# CMake is used ONLY because gRPC links via host pkg-config (grpc++) —
# the rest of the system is Bazel. `bazel build //services/log:services-log`
# wraps this via rules_foreign_cc cmake() (see BUILD.bazel); direct
# `cmake -S . -B build` also works for dev.

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

# nlohmann/json — single-header, vendored under third_party/. Bazel passes
# -DNLOHMANN_JSON_INCLUDE_DIR; the direct CMake build defaults to the repo
# path (services/log → repo root is ../..).
set(NLOHMANN_JSON_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nlohmann_json"
    CACHE PATH "nlohmann/json single-header include dir")

# Proto sources. TraceRecord lives in supervisor_bridge.proto (services/com),
# which imports several supervisor protos; the TraceStream SERVICE lives in
# services/log/proto/trace_stream.proto (imports supervisor_bridge).
set(SUP_PROTO_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/supervisor/generated/proto)
set(COM_PROTO_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../com/proto)
set(LOG_PROTO_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# Supervisor protos referenced (transitively) by supervisor_bridge.proto.
# Same set com pulls — supervisor_bridge imports these.
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
    add_custom_command(
        OUTPUT ${PROTO_GEN_DIR}/${P}.pb.cc ${PROTO_GEN_DIR}/${P}.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${SUP_PROTO_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                ${SUP_PROTO_DIR}/${P}.proto
        DEPENDS ${SUP_PROTO_DIR}/${P}.proto
        COMMENT "protoc ${P}.proto"
        VERBATIM
    )
    list(APPEND SUP_PROTO_SRCS ${PROTO_GEN_DIR}/${P}.pb.cc)
endforeach()

# supervisor_bridge.proto — message types (incl. TraceRecord) + grpc stubs
# (we don't use its services, but generating grpc is harmless + matches com).
add_custom_command(
    OUTPUT  ${PROTO_GEN_DIR}/supervisor_bridge.pb.cc
            ${PROTO_GEN_DIR}/supervisor_bridge.pb.h
            ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.cc
            ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.h
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

# trace_stream.proto — the TraceStream SERVICE the collector serves. Imports
# supervisor_bridge.proto for TraceRecord, so -I both dirs.
add_custom_command(
    OUTPUT  ${PROTO_GEN_DIR}/trace_stream.pb.cc
            ${PROTO_GEN_DIR}/trace_stream.pb.h
            ${PROTO_GEN_DIR}/trace_stream.grpc.pb.cc
            ${PROTO_GEN_DIR}/trace_stream.grpc.pb.h
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${LOG_PROTO_DIR}
            --proto_path=${COM_PROTO_DIR}
            --proto_path=${SUP_PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${LOG_PROTO_DIR}/trace_stream.proto
    DEPENDS ${LOG_PROTO_DIR}/trace_stream.proto
            ${PROTO_GEN_DIR}/supervisor_bridge.pb.h
    COMMENT "protoc (+grpc) trace_stream.proto"
    VERBATIM
)

add_executable(services-log
    src/main.cpp
    ${SUP_PROTO_SRCS}
    ${PROTO_GEN_DIR}/supervisor_bridge.pb.cc
    ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.cc
    ${PROTO_GEN_DIR}/trace_stream.pb.cc
    ${PROTO_GEN_DIR}/trace_stream.grpc.pb.cc
)

target_include_directories(services-log PRIVATE
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)

target_link_libraries(services-log PRIVATE
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    -lgrpc -lgpr
    pthread
)

target_compile_options(services-log PRIVATE
    -Wno-unused-parameter
    -Wno-pedantic
)

install(TARGETS services-log RUNTIME DESTINATION bin)
