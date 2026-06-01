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

# ---------------------------------------------------------------------------
# #417 — standard-transport control surface (nanopb). control_node.cpp +
# ControlServer use the platform/runtime gen_server stack (GenServer.hh,
# TipcMux.hh, NodeRef.hh, RemoteCodec.hh) and the NANOPB supervisor binding
# (ControlRequest.pb.h from //platform/supervisor:supervisor_nanopb). It must
# be a SEPARATE object library so its include path carries the nanopb
# ControlRequest.pb.h — NOT the libprotobuf one in ${PROTO_GEN_DIR} that
# runtime.cpp/supervisor_proto compile against. Same-basename headers, kept in
# separate TUs (see src/control_node.cpp + include/supervisor/control_server.h).
#
# Under the Bazel cmake() wrapper the runtime library + nanopb headers/libs are
# staged into $EXT_BUILD_DEPS via deps=[]; for a standalone `cmake -S . -B
# build` dev tree they resolve through the in-tree sibling paths below.
set(RUNTIME_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/runtime")
set(LIBGW_INC_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway/libs/libgw/include")
set(SUP_NANOPB_INC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_nanopb")

# $EXT_BUILD_DEPS is set by rules_foreign_cc; empty for a standalone build.
set(EXT_DEPS "$ENV{EXT_BUILD_DEPS}")

# Standalone dev build: generate the nanopb supervisor headers (Bazel's
# supervisor_nanopb genrule does this in the sandbox + stages them into
# $EXT_BUILD_DEPS/include). Only the control-path .pb.h are needed for the
# include path; the .pb.c we compile for the linked symbols.
set(SUP_NANOPB_SRCS "")
if(EXT_DEPS STREQUAL "")
    find_program(NANOPB_GENERATOR nanopb_generator)
    set(SUP_NANOPB_PROTOS
        ControlRequest ControlReply
        StartChildRequest DeleteChildRequest ChildSelector
        GetChildRequest GetTreeRequest GetSystemInfoRequest
        ConfigureTraceRequest GetTraceConfigRequest TraceConfig
        TraceConfigList ConfigureLogLevelRequest LogLevelConfig
        ChildSpec HeartbeatReport SendTimeoutReport)
    file(MAKE_DIRECTORY ${SUP_NANOPB_INC_DIR})
    if(NANOPB_GENERATOR)
        set(SUP_NANOPB_PROTO_ARGS "")
        foreach(P ${SUP_NANOPB_PROTOS})
            list(APPEND SUP_NANOPB_PROTO_ARGS ${PROTO_SRC_DIR}/${P}.proto)
            list(APPEND SUP_NANOPB_SRCS ${SUP_NANOPB_INC_DIR}/${P}.pb.c)
        endforeach()
        add_custom_command(
            OUTPUT ${SUP_NANOPB_SRCS}
            COMMAND ${NANOPB_GENERATOR}
                    -I ${PROTO_SRC_DIR}
                    -f ${CMAKE_CURRENT_SOURCE_DIR}/system/supervisor.options
                    -D ${SUP_NANOPB_INC_DIR}
                    --no-error-on-unmatched
                    ${SUP_NANOPB_PROTO_ARGS}
            DEPENDS ${PROTO_FILES}
            COMMENT "nanopb_generator supervisor control protos"
            VERBATIM)
    else()
        message(WARNING "nanopb_generator not found; standalone control-node "
                        "build will fail. Bazel cmake() stages these headers.")
    endif()
endif()

add_library(supervisor_control_node OBJECT
    src/control_node.cpp
    ${SUP_NANOPB_SRCS}
)
target_compile_options(supervisor_control_node PRIVATE
    # The platform/runtime gen_server stack is C++17 (inline-vars +
    # if-constexpr); override the project-wide C++14 for this TU only.
    # In particular RemoteCodec<T>::service_id is a constexpr static that
    # is implicitly inline (no out-of-line def) only under C++17.
    -std=c++17
    -fPIC
    -Wno-unused-parameter
)
set_target_properties(supervisor_control_node PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(supervisor_control_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # platform/runtime gen_server headers + the runtime nanopb proto root
    # (platform_runtime/runtime.pb.h) + libgw (gw_proto.h).
    ${RUNTIME_DIR}/include
    ${RUNTIME_DIR}/proto
    ${LIBGW_INC_DIR}
    # nanopb supervisor binding (ControlRequest.pb.h etc.). Bazel stages it
    # into $EXT_BUILD_DEPS/include; standalone build generates it below.
    ${SUP_NANOPB_INC_DIR}
    ${EXT_DEPS}/include
)

add_executable(supervisor
    src/main.cpp
    src/runtime.cpp
    src/spec.cpp
    src/tipc_publisher.cpp
    src/etcd_publisher.cpp
    $<TARGET_OBJECTS:supervisor_proto>
    $<TARGET_OBJECTS:supervisor_control_node>
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

# ---------------------------------------------------------------------------
# #417 — link the platform/runtime gen_server stack + nanopb runtime that the
# control node needs (TipcMux::bind_node, TipcClient, tracer_for,
# process_logger, the LogLevelPush nanopb codec, and the supervisor nanopb
# control structs).
#
#   * Bazel cmake(): the built libraries are staged into $EXT_BUILD_DEPS/lib
#     by deps=["//platform/runtime:runtime", ":supervisor_nanopb"]. We link
#     the staged archives + the host nanopb runtime.
#   * Standalone dev: compile the handful of runtime sources directly + the
#     runtime nanopb codec; the supervisor nanopb .pb.c rode into the
#     control-node object lib above.
#
# Either way the host nanopb runtime (libprotobuf-nanopb.a) supplies
# pb_encode/pb_decode.
if(NOT EXT_DEPS STREQUAL "")
    # Bazel: link the staged STATIC archives. rules_foreign_cc stages BOTH
    # lib<rule>.a and lib<rule>.so under $EXT_BUILD_DEPS/lib; find_library's
    # default suffix order prefers .so, which would leave the deployed binary
    # with an unresolvable libruntime.so / libsupervisor_nanopb.so at runtime
    # (no rpath). Pin the search to .a so we statically link them — same
    # static-only policy as supervisor_pb_cpp.a / libprotobuf-nanopb.a.
    set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(RUNTIME_LIB        NAMES runtime libruntime
                 PATHS ${EXT_DEPS}/lib NO_DEFAULT_PATH)
    find_library(SUP_NANOPB_LIB     NAMES supervisor_nanopb libsupervisor_nanopb
                 PATHS ${EXT_DEPS}/lib NO_DEFAULT_PATH)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
    target_link_libraries(supervisor PRIVATE
        ${RUNTIME_LIB}
        ${SUP_NANOPB_LIB}
        -l:libprotobuf-nanopb.a
    )
    # gw_proto.h / runtime headers were copied to $EXT_BUILD_DEPS/include by
    # the staged deps; control_node.cpp already includes them via the object
    # lib's include dirs. supervisor's own TUs don't need them.
else()
    # Standalone: compile the minimal runtime sources the control node pulls.
    target_sources(supervisor PRIVATE
        ${RUNTIME_DIR}/src/TipcMux.cc
        ${RUNTIME_DIR}/src/NodeRef.cc
        ${RUNTIME_DIR}/src/Logger.cc
        ${RUNTIME_DIR}/proto/platform_runtime/runtime.pb.c
    )
    set_source_files_properties(
        ${RUNTIME_DIR}/src/TipcMux.cc
        ${RUNTIME_DIR}/src/NodeRef.cc
        ${RUNTIME_DIR}/src/Logger.cc
        PROPERTIES COMPILE_OPTIONS "-std=c++17;-Wno-unused-parameter")
    target_include_directories(supervisor PRIVATE
        ${RUNTIME_DIR}/include
        ${RUNTIME_DIR}/proto
        ${LIBGW_INC_DIR})
    target_link_libraries(supervisor PRIVATE -l:libprotobuf-nanopb.a)
endif()

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

# crasher + tombstone are the crash-forensics demo/lib. Install them for a
# standalone `cmake --install` (dev + the .ipk packaging), but NOT under the
# Bazel cmake() wrapper, which builds targets=["supervisor"] only — a stray
# install(TARGETS crasher) there fails because crasher was never built.
# Flip OFF via -DINSTALL_SUPERVISOR_EXTRAS=OFF (the Bazel cmake() does).
option(INSTALL_SUPERVISOR_EXTRAS "Install crasher + tombstone alongside the supervisor" ON)
if(INSTALL_SUPERVISOR_EXTRAS)
    install(TARGETS crasher RUNTIME DESTINATION bin)
    install(TARGETS tombstone ARCHIVE DESTINATION lib)
endif()


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
