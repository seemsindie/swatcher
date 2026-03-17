cmake_minimum_required(VERSION 3.20.0)
project(swatcher VERSION 0.1.0 LANGUAGES C)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

# C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Core sources (always compiled)
set(CORE_SOURCES
    src/core/swatcher.c
    src/core/log.c
    src/core/event.c
    src/core/target.c
    src/core/pattern.c
    src/regex/re.c
)

# Platform sources
if(WIN32)
    set(PLATFORM_SOURCES src/platform/platform_windows.c)
else()
    set(PLATFORM_SOURCES src/platform/platform_posix.c)
endif()

# Backend sources (poll fallback is always compiled)
set(BACKEND_SOURCES src/backend/backend_poll.c)
if(WIN32)
    list(APPEND BACKEND_SOURCES src/backend/backend_win32.c)
elseif(APPLE)
    list(APPEND BACKEND_SOURCES src/backend/backend_fsevents.c src/backend/backend_kqueue.c)
else()
    list(APPEND BACKEND_SOURCES src/backend/backend_inotify.c)
endif()

add_library(swatcher
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${BACKEND_SOURCES}
)

target_include_directories(swatcher PUBLIC include)
target_include_directories(swatcher PRIVATE src)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(swatcher PRIVATE SWATCHER_BUILD_DLL=1)
    set_target_properties(swatcher PROPERTIES OUTPUT_NAME swatcher)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(swatcher PRIVATE /W4)
else()
    target_compile_options(swatcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Debug/Release flags
target_compile_options(swatcher PRIVATE
    "$<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:DEBUG>>:-g>"
    "$<$<AND:$<C_COMPILER_ID:GNU>,$<CONFIG:RELEASE>>:-O3>"
    "$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:DEBUG>>:/Zi>"
    "$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:RELEASE>>:/O2>"
)

# Link pthreads on POSIX
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(swatcher PRIVATE Threads::Threads)
endif()

# Link CoreServices on macOS (FSEvents)
if(APPLE)
    target_link_libraries(swatcher PRIVATE "-framework CoreServices")
endif()

# Example
add_executable(swatcher_first examples/swatcher_first.c)
target_link_libraries(swatcher_first swatcher)

# Pattern tests
add_executable(test_pattern tests/test_pattern.c src/core/pattern.c src/regex/re.c src/core/log.c)
target_include_directories(test_pattern PRIVATE include src)

# Platform tests
if(WIN32)
    set(TEST_PLATFORM_SOURCES src/platform/platform_windows.c)
else()
    set(TEST_PLATFORM_SOURCES src/platform/platform_posix.c)
endif()
add_executable(test_platform tests/test_platform.c ${TEST_PLATFORM_SOURCES})
target_include_directories(test_platform PRIVATE include src)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_platform PRIVATE Threads::Threads)
endif()

# Poll backend tests (links full library)
add_executable(test_poll tests/test_poll.c)
target_link_libraries(test_poll swatcher)

# Poll backend benchmark
add_executable(bench_poll tests/bench_poll.c)
target_link_libraries(bench_poll swatcher)

# inotify backend tests (Linux only)
if(NOT WIN32 AND NOT APPLE)
    add_executable(test_inotify tests/test_inotify.c)
    target_link_libraries(test_inotify swatcher)

    add_executable(stress_inotify tests/stress_inotify.c)
    target_link_libraries(stress_inotify swatcher)
endif()

# kqueue backend tests (macOS/BSD only)
if(APPLE)
    add_executable(test_kqueue tests/test_kqueue.c)
    target_link_libraries(test_kqueue swatcher)

    add_executable(test_fsevents tests/test_fsevents.c)
    target_link_libraries(test_fsevents swatcher)
endif()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
