cmake_minimum_required(VERSION 3.20.0)
project(swatcher VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
enable_testing()

# Option for building shared (dynamic) or static library
option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

# Store the source files in a variable
set(SOURCE_FILES src/swatcher.c)

# Include directories
include_directories(include)

# Create the static library
add_library(swatcher STATIC ${SOURCE_FILES})

# Create the shared library (DLL)
add_library(swatcher_shared SHARED ${SOURCE_FILES})
set_target_properties(swatcher_shared PROPERTIES OUTPUT_NAME swatcher)
# Define SWATCHER_BUILD_DLL when building the shared library
# target_compile_definitions(swatcher_shared PRIVATE SWATCHER_BUILD_DLL=1)
# Set properties for different build types
# set(CMAKE_C_FLAGS_DEBUG "-g")
# set(CMAKE_C_FLAGS_RELEASE "-O3")

# if(MSVC)
#   set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi")
#   set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2")
# endif()
target_compile_options(swatcher PRIVATE
  "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<CONFIG:DEBUG>>:-g>"
  "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<CONFIG:RELEASE>>:-O3>"
  "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:DEBUG>>:/Zi>"
  "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:RELEASE>>:/O2>"
)


# swatcher first example
add_executable(swatcher_first examples/swatcher_first.c)
target_link_libraries(swatcher_first swatcher)

add_executable(windows_example examples/windows_example.c)
target_link_libraries(windows_example swatcher)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
