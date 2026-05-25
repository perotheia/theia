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
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json"
    CACHE PATH "Path containing nlohmann/json.hpp")

find_package(wxWidgets REQUIRED COMPONENTS core base aui)
include(${wxWidgets_USE_FILE})

# etcd-cpp-apiv3 — vendored under third_party/ via repo manifest, built
# + installed to third_party/etcd-cpp-apiv3/install/ (see that dir's
# README). Pick it up via the CMake config exported there. If a future
# system-wide install ever ships, CMAKE_PREFIX_PATH on the cmdline can
# override.
set(ETCD_CPP_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/etcd-cpp-apiv3/install"
    CACHE PATH "etcd-cpp-apiv3 install prefix")
find_package(etcd-cpp-api REQUIRED CONFIG
    HINTS "${ETCD_CPP_PREFIX}/lib/cmake/etcd-cpp-api")

find_program(PROTOC_EXECUTABLE protoc           REQUIRED)
find_program(GRPC_CPP_PLUGIN   grpc_cpp_plugin  REQUIRED)

# Supervisor's .proto pile — wire-side message types.
set(SUP_PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../platform/supervisor/generated/proto)
# services/com's bridge .proto — gRPC service surface.
set(COM_PROTO_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../services/com/proto)
set(PROTO_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# All supervisor protos.
file(GLOB SUP_PROTO_FILES ${SUP_PROTO_DIR}/*.proto)
set(SUP_PROTO_GEN_SRCS)
foreach(PROTO_FILE ${SUP_PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PB_CC ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(PB_H  ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h)
    add_custom_command(
        OUTPUT ${PB_CC} ${PB_H}
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${SUP_PROTO_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "protoc ${PROTO_NAME}.proto"
        VERBATIM
    )
    list(APPEND SUP_PROTO_GEN_SRCS ${PB_CC})
endforeach()

# Bridge .proto — message types + gRPC client stubs.
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
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${COM_PROTO_DIR}/supervisor_bridge.proto
    DEPENDS ${COM_PROTO_DIR}/supervisor_bridge.proto
    COMMENT "protoc (+grpc) supervisor_bridge.proto"
    VERBATIM
)

add_library(supervisor_gui_proto OBJECT
    ${SUP_PROTO_GEN_SRCS}
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
    src/panels/system_panel.cpp
    src/panels/load_charts_panel.cpp
    src/panels/applications_panel.cpp
    src/panels/processes_panel.cpp
    src/panels/etcd_panel.cpp
    src/panels/machines_panel.cpp
    src/panels/trace_panel.cpp
    $<TARGET_OBJECTS:supervisor_gui_proto>
)

target_include_directories(supervisor-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${PROTO_GEN_DIR}
    # etcd-cpp-api's exported targets don't carry
    # INTERFACE_INCLUDE_DIRECTORIES (upstream config issue), so we feed
    # the install prefix's include/ manually. Drop this when upstream
    # fixes the target export, or when we switch to a CMake-superbuild
    # that has full propagation.
    ${ETCD_CPP_PREFIX}/include
)

target_link_libraries(supervisor-gui PRIVATE
    ${wxWidgets_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    ${GRPCPP_LIBRARIES}
    -lgrpc -lgpr
    # etcd-cpp-apiv3's CMake config exports targets like etcd-cpp-api;
    # those bring along their own transitive cpprestsdk / boost / openssl
    # link dependencies via INTERFACE_LINK_LIBRARIES.
    etcd-cpp-api
    pthread
)

install(TARGETS supervisor-gui RUNTIME DESTINATION bin)
