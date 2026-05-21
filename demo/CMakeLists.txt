# gen_server demo project — a reference / scratch app.
#
# Pulls the canonical runtime from platform/runtime/. Owns its own
# nodes, proto, and per-process main binaries. The canonical test
# suite for the runtime now lives at platform/runtime/test/ — built
# via Bazel (cc_test) — this CMake project does not duplicate it.

cmake_minimum_required(VERSION 3.16)
project(gen_server_demo C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Runtime sources live in platform/runtime/ (the canonical location).
set(PLATFORM_RUNTIME ${CMAKE_SOURCE_DIR}/../platform/runtime)
set(RUNTIME_SRCS
    ${PLATFORM_RUNTIME}/src/Clock.cc
    ${PLATFORM_RUNTIME}/src/Logger.cc
    ${PLATFORM_RUNTIME}/src/TimerService.cc
    ${PLATFORM_RUNTIME}/src/NodeRef.cc
    ${PLATFORM_RUNTIME}/src/TipcMux.cc
)

set(PROTO_SRCS
    proto/demo/system/system.pb.c
)
set_source_files_properties(${PROTO_SRCS} PROPERTIES LANGUAGE C)

set(NODE_SRCS
    src/counter_node.cc
    src/ticker_node.cc
)
# DriverNode, ObserverNode, IncrementerNode are class templates now —
# their definitions live in <node>.tcc, included from the .hh. Each
# instantiation lands in the TU that names it (main, p1_main, etc.).

# Library so all binaries share the runtime + nodes + protos.
add_library(demo_runtime STATIC
    ${RUNTIME_SRCS}
    ${PROTO_SRCS}
    ${NODE_SRCS}
)
target_include_directories(demo_runtime PUBLIC
    src
    ${PLATFORM_RUNTIME}/include
    proto
    /usr/include
)
# libgw's gw_proto.h lives one repo over; platform/runtime/include/NodeRef.hh
# refers to it via plain `#include "gw_proto.h"`.
target_include_directories(demo_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/../gateway/libs/libgw/include
)
target_link_libraries(demo_runtime PUBLIC
    pthread
    -l:libprotobuf-nanopb.a
)

# Single-process demo (the existing one — unchanged externally).
add_executable(${PROJECT_NAME}
    src/main.cc
)
target_link_libraries(${PROJECT_NAME} PRIVATE demo_runtime)

# Three-process variant — one binary per role.
add_executable(demo_p1 src/p1_main.cc)
target_link_libraries(demo_p1 PRIVATE demo_runtime)

add_executable(demo_p2 src/p2_main.cc)
target_link_libraries(demo_p2 PRIVATE demo_runtime)

add_executable(demo_p3 src/p3_main.cc)
target_link_libraries(demo_p3 PRIVATE demo_runtime)

# The runtime test suite is owned by platform/runtime/ (Bazel cc_test).
# This demo project no longer builds its own copy — eliminates the
# previous divergent maintenance of two test files.

# ---- Generated apps from `artheia gen-app-composition Demo3Way` ---------
# Each subdir provides one binary (gen_Demo3Way_P{1,2,3}) sharing the
# demo_runtime library. Built alongside the handwritten demo_p* so a
# 3-process e2e test (running outside CMake) can compare them.
foreach(P P1 P2 P3)
    set(_gen_dir ${CMAKE_SOURCE_DIR}/../applications/demo_composition/Demo3Way_${P})
    if(EXISTS ${_gen_dir}/main.cc)
        add_executable(gen_Demo3Way_${P} ${_gen_dir}/main.cc)
        target_link_libraries(gen_Demo3Way_${P} PRIVATE demo_runtime)
    endif()
endforeach()
