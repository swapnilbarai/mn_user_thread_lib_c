cmake_minimum_required(VERSION 3.16)

project(
    mn_uthread
    VERSION 1.0
    LANGUAGES C CXX ASM
)

# ---------------------------------------------------------------------------
# Platform / architecture
# ---------------------------------------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "mn_uthread requires Linux")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    message(FATAL_ERROR "mn_uthread requires x86-64 architecture")
endif()

message(STATUS "System      : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")


# ---------------------------------------------------------------------------
# Language standards
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)


# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------

set(COMMON_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
)


# ---------------------------------------------------------------------------
# uthread library
# ---------------------------------------------------------------------------

add_library(
    uthread_pool
    STATIC
        src/uthread.c
        src/context.S
)

target_include_directories(
    uthread_pool
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include/public
)

target_compile_definitions(
    uthread_pool
    PRIVATE
        _GNU_SOURCE
)

target_compile_options(
    uthread_pool
    PRIVATE
        ${COMMON_WARNING_FLAGS}
        -fno-omit-frame-pointer
)

target_link_libraries(
    uthread_pool
    PUBLIC
        Threads::Threads
)


# ---------------------------------------------------------------------------
# Test: yield / join
# ---------------------------------------------------------------------------

add_executable(
    test_yield_join
    src/main.c
)

target_compile_options(
    test_yield_join
    PRIVATE
        ${COMMON_WARNING_FLAGS}
        -fno-omit-frame-pointer
)

target_link_libraries(
    test_yield_join
    PRIVATE
        uthread_pool
)


# ---------------------------------------------------------------------------
# Test: MPMC
# ---------------------------------------------------------------------------

add_executable(
    test_mp_mc
    tests/mpmc.cpp
)

target_include_directories(
    test_mp_mc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/private
)

target_compile_options(
    test_mp_mc
    PRIVATE
        ${COMMON_WARNING_FLAGS}
        -fno-omit-frame-pointer
)

target_link_libraries(
    test_mp_mc
    PRIVATE
        uthread_pool
)


# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------

enable_testing()

add_test(
    NAME yield_join
    COMMAND test_yield_join
)

add_test(
    NAME mpmc
    COMMAND test_mp_mc
)