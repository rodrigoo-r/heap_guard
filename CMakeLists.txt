include(FetchContent)

cmake_minimum_required(VERSION 3.12)
project(heap_guard C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_library(heap_guard STATIC "heap_guard.c" "heap_guard.h")

if(NOT FLUENT_LIBC_RELEASE) # Manually add libraries only if not in release mode
    FetchContent_Declare(
            mutex
            GIT_REPOSITORY https://github.com/rodrigoo-r/mutex
            GIT_TAG        master
    )

    FetchContent_Declare(
            atomic
            GIT_REPOSITORY https://github.com/rodrigoo-r/atomic
            GIT_TAG        master
    )

    FetchContent_Declare(
            arena
            GIT_REPOSITORY https://github.com/rodrigoo-r/arena
            GIT_TAG        master
    )

    FetchContent_MakeAvailable(arena)
    FetchContent_MakeAvailable(atomic)
    FetchContent_MakeAvailable(mutex)

    target_include_directories(heap_guard PRIVATE ${CMAKE_BINARY_DIR}/_deps/mutex-src)
    target_link_libraries(heap_guard PRIVATE mutex)
    target_include_directories(heap_guard PRIVATE ${CMAKE_BINARY_DIR}/_deps/atomic-src)
    target_link_libraries(heap_guard PRIVATE atomic)
    target_include_directories(heap_guard PRIVATE ${CMAKE_BINARY_DIR}/_deps/arena-src)
    target_link_libraries(heap_guard PRIVATE arena)
endif ()

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