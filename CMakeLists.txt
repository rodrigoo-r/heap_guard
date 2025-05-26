include(FetchContent)

cmake_minimum_required(VERSION 3.30)
project(heap_guard C)

FetchContent_Declare(
    mutex
    GIT_REPOSITORY https://github.com/rodrigoo-r/mutex
    GIT_TAG        master
)

FetchContent_Declare(
    hashmap
    GIT_REPOSITORY https://github.com/rodrigoo-r/hashmap
    GIT_TAG        master
)

FetchContent_MakeAvailable(mutex)
FetchContent_MakeAvailable(hashmap)
set(CMAKE_C_STANDARD 11)

add_library(heap_guard STATIC "heap_guard.c")
target_include_directories(heap_guard PRIVATE ${CMAKE_BINARY_DIR}/_deps/mutex-src)
target_link_libraries(heap_guard PRIVATE mutex)
target_include_directories(heap_guard PRIVATE ${CMAKE_BINARY_DIR}/_deps/hashmap-src)
target_link_libraries(heap_guard PRIVATE hashmap)

# Jemalloc
if(NOT WIN32) # Guard against Windows, falls to libc on Windows
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JEMALLOC QUIET jemalloc)
    if(JEMALLOC_FOUND)
        target_compile_definitions(heap_guard PUBLIC DEBUG HAVE_JEMALLOC=1)
        message(STATUS "jemalloc found! Adding it to the link libraries.")
        target_include_directories(heap_guard PRIVATE ${JEMALLOC_INCLUDE_DIRS})
        target_link_libraries(heap_guard PRIVATE ${JEMALLOC_LIBRARIES})
    else()
        message(STATUS "jemalloc NOT found, skipping.")
    endif()
endif()