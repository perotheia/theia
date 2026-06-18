cmake_minimum_required(VERSION 3.16)
project(supervisor_gui CXX)

# C++17 needed for the gRPC client API.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(GRPCPP   REQUIRED grpc++)

# nlohmann/json — header-only, vendored under third_party/ (see #380).
# Replaces yaml-cpp now that manifests are JSON-only.
set(NLOHMANN_JSON_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nlohmann_json"
    CACHE PATH "Path containing nlohmann/json.hpp")

find_package(wxWidgets REQUIRED COMPONENTS core base aui)
include(${wxWidgets_USE_FILE})

# NOTE: the GUI no longer links etcd-cpp-apiv3. The Table Viewer reads per's
# config store over com→per (PerView.GetSnapshot) and decodes client-side —
# etcd lives in the DMZ and services/per is the sole etcd client. No
# LD_LIBRARY_PATH to the etcd .so is required to run the GUI.

find_program(PROTOC_EXECUTABLE protoc           REQUIRED)
find_program(GRPC_CPP_PLUGIN   grpc_cpp_plugin  REQUIRED)

# Proto roots — mirror services/com's genrule (the GUI links the SAME wire).
# The supervisor messages were consolidated (psp-retirement) into ONE proto,
# package `system_supervisor`, importing the runtime proto for shared enums.
#   supervisor_bridge.proto  → import "supervisor.proto"      (the gRPC service)
#   supervisor.proto         → import "platform_runtime/runtime.proto"
set(COM_PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../services/com/proto)
set(SUP_PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/proto/system/supervisor)
set(RT_PROTO_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/runtime/proto)
set(PROTO_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# runtime.proto (package platform_runtime) — shared enums (LogLevelValue,
# TraceKind). Keep its platform_runtime/ path prefix so the import resolves.
set(RT_PB_CC ${PROTO_GEN_DIR}/platform_runtime/runtime.pb.cc)
set(RT_PB_H  ${PROTO_GEN_DIR}/platform_runtime/runtime.pb.h)
add_custom_command(
    OUTPUT ${RT_PB_CC} ${RT_PB_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${RT_PROTO_ROOT}
            --cpp_out=${PROTO_GEN_DIR}
            ${RT_PROTO_ROOT}/platform_runtime/runtime.proto
    DEPENDS ${RT_PROTO_ROOT}/platform_runtime/runtime.proto
    COMMENT "protoc runtime.proto"
    VERBATIM
)

# supervisor.proto (package system_supervisor) — TreeSnapshot, HealthBeacon,
# SupervisionEvent, ControlReply, … the consolidated wire-side message types.
set(SUP_PB_CC ${PROTO_GEN_DIR}/supervisor.pb.cc)
set(SUP_PB_H  ${PROTO_GEN_DIR}/supervisor.pb.h)
add_custom_command(
    OUTPUT ${SUP_PB_CC} ${SUP_PB_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${SUP_PROTO_DIR}
            --proto_path=${RT_PROTO_ROOT}
            --cpp_out=${PROTO_GEN_DIR}
            ${SUP_PROTO_DIR}/supervisor.proto
    DEPENDS ${SUP_PROTO_DIR}/supervisor.proto ${RT_PB_H}
    COMMENT "protoc supervisor.proto"
    VERBATIM
)

# Bridge .proto — gRPC service surface (SupervisorView + TraceStream) +
# the services.com bridge messages. Imports supervisor.proto (system_supervisor).
set(BRIDGE_PB_CC   ${PROTO_GEN_DIR}/supervisor_bridge.pb.cc)
set(BRIDGE_PB_H    ${PROTO_GEN_DIR}/supervisor_bridge.pb.h)
set(BRIDGE_GRPC_CC ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.cc)
set(BRIDGE_GRPC_H  ${PROTO_GEN_DIR}/supervisor_bridge.grpc.pb.h)
add_custom_command(
    OUTPUT  ${BRIDGE_PB_CC} ${BRIDGE_PB_H}
            ${BRIDGE_GRPC_CC} ${BRIDGE_GRPC_H}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${COM_PROTO_DIR}
            --proto_path=${SUP_PROTO_DIR}
            --proto_path=${RT_PROTO_ROOT}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${COM_PROTO_DIR}/supervisor_bridge.proto
    DEPENDS ${COM_PROTO_DIR}/supervisor_bridge.proto ${SUP_PB_H}
    COMMENT "protoc (+grpc) supervisor_bridge.proto"
    VERBATIM
)

add_library(supervisor_gui_proto OBJECT
    ${RT_PB_CC}
    ${SUP_PB_CC}
    ${BRIDGE_PB_CC}
    ${BRIDGE_GRPC_CC}
)
target_include_directories(supervisor_gui_proto PUBLIC ${PROTO_GEN_DIR})
target_compile_options(supervisor_gui_proto PRIVATE
    ${PROTOBUF_CFLAGS_OTHER}
    -Wno-unused-parameter
    -Wno-pedantic
)
target_link_libraries(supervisor_gui_proto PUBLIC ${PROTOBUF_LIBRARIES})

add_executable(supervisor-gui
    src/main.cpp
    src/main_frame.cpp
    src/grpc_client.cpp
    src/machines.cpp
    src/trace_decoder_lib.cpp
    src/panels/system_panel.cpp
    src/panels/load_charts_panel.cpp
    src/panels/applications_panel.cpp
    src/panels/processes_panel.cpp
    src/panels/etcd_panel.cpp
    src/panels/persistency_panel.cpp
    src/panels/machines_panel.cpp
    src/panels/trace_panel.cpp
    src/panels/log_panel.cpp
    src/panels/health_panel.cpp
    $<TARGET_OBJECTS:supervisor_gui_proto>
)

target_include_directories(supervisor-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${PROTO_GEN_DIR}
)

target_link_libraries(supervisor-gui PRIVATE
    ${wxWidgets_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    -lgrpc -lgpr
    pthread
    # dlopen/dlsym for libtrace_decoder_ctypes.so (trace_decoder_lib.cpp).
    ${CMAKE_DL_LIBS}
)

install(TARGETS supervisor-gui RUNTIME DESTINATION bin)
