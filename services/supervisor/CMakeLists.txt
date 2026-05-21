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
pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)

add_executable(supervisor
    src/main.cpp
    src/runtime.cpp
    src/spec.cpp
)

target_include_directories(supervisor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${YAML_CPP_INCLUDE_DIRS}
)

target_link_libraries(supervisor PRIVATE
    ${YAML_CPP_LIBRARIES}
    pthread
)

target_compile_options(supervisor PRIVATE ${YAML_CPP_CFLAGS_OTHER})

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
