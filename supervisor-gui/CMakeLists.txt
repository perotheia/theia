cmake_minimum_required(VERSION 3.16)
project(supervisor_gui CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

# wxWidgets — try the 3.2 first, fall back to 3.0.
find_package(wxWidgets REQUIRED COMPONENTS core base aui)
include(${wxWidgets_USE_FILE})

# .proto compilation — the supervisor's .art is the source of truth for
# the wire schema. We point at the supervisor's gen-proto outputs so
# the GUI compiles the same exact messages without a copy step.
find_program(PROTOC_EXECUTABLE protoc REQUIRED)

set(PROTO_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../services/supervisor/generated/proto)
set(PROTO_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

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

add_library(supervisor_gui_proto OBJECT ${PROTO_GEN_SRCS})
target_include_directories(supervisor_gui_proto PUBLIC ${PROTO_GEN_DIR})
target_compile_options(supervisor_gui_proto PRIVATE
    ${PROTOBUF_CFLAGS_OTHER}
    -Wno-unused-parameter
    -Wno-pedantic
)
target_link_libraries(supervisor_gui_proto PUBLIC ${PROTOBUF_LIBRARIES})

# Main GUI binary.
add_executable(supervisor-gui
    src/main.cpp
    src/main_frame.cpp
    src/tipc_client.cpp
    src/panels/tree_panel.cpp
    src/panels/process_panel.cpp
    src/panels/trace_panel.cpp
    src/panels/tombstone_panel.cpp
    src/panels/perf_panel.cpp
    $<TARGET_OBJECTS:supervisor_gui_proto>
)

target_include_directories(supervisor-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOBUF_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
)

target_link_libraries(supervisor-gui PRIVATE
    ${wxWidgets_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    pthread
)

install(TARGETS supervisor-gui RUNTIME DESTINATION bin)

# Headless smoke-test probe (no wx). Same TIPC client, prints frames to
# stderr. Useful in CI / containers without a display server.
add_executable(tipc_probe
    src/tipc_probe.cpp
    src/tipc_client.cpp
)
target_include_directories(tipc_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(tipc_probe PRIVATE pthread)
