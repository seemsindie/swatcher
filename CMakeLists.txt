cmake_minimum_required(VERSION 3.20.0)
project(swatcher VERSION 0.1.0 LANGUAGES C)

include(CTest)
enable_testing()

# Option for building shared (dynamic) or static library
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

# Store the source files in a variable
set(SOURCE_FILES src/swatcher.c)

# Include directories
include_directories(include)

add_library(swatcher ${SOURCE_FILES})

# Set properties for different build types
set(CMAKE_C_FLAGS_DEBUG "-g")
set(CMAKE_C_FLAGS_RELEASE "-O3")

if(MSVC)
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2")
endif()

# swatcher first example
add_executable(swatcher_first examples/swatcher_first.c)
target_link_libraries(swatcher_first swatcher)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
