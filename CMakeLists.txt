cmake_minimum_required(VERSION 3.25)

project(p2ep VERSION 0.1.0 LANGUAGES C CXX)

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    message(FATAL_ERROR "p2ep is built with clang-cl. Use scripts/build.ps1 or one of the CMake presets.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(P2EP_BUILD_TESTS "Build the unit tests" ON)
option(P2EP_LTO "Link the game with ThinLTO" OFF)
option(P2EP_DEV "Developer build: guest backtraces, extra checks, validation layers" OFF)
set(P2EP_IMAGE "" CACHE FILEPATH "Path to your disc image of the game. Enables the recompile step and the game target.")
set(P2EP_SHARDS 96 CACHE STRING "Number of generated translation units")
set(P2EP_ICON "" CACHE FILEPATH "Image (PNG or JPEG) for the program's icon. Default: the first image in icon/.")

# Shared warning and codegen settings for first-party targets.
add_library(p2ep_options INTERFACE)
target_compile_definitions(p2ep_options INTERFACE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
    $<$<BOOL:${P2EP_DEV}>:P2EP_DEV=1>)
target_compile_options(p2ep_options INTERFACE
    /W4 /permissive- /utf-8 /Zc:inline /GR- /EHsc
    -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
    -Wno-gnu-zero-variadic-macro-arguments -Wno-nested-anon-types -Wno-gnu-anonymous-struct
    -Werror=return-type -Werror=implicit-fallthrough)
target_include_directories(p2ep_options INTERFACE "${CMAKE_SOURCE_DIR}")

add_subdirectory(lib)
add_subdirectory(tools/recomp)
add_subdirectory(tools/icon)

if(P2EP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(P2EP_IMAGE)
    if(NOT EXISTS "${P2EP_IMAGE}")
        message(FATAL_ERROR "P2EP_IMAGE does not exist: ${P2EP_IMAGE}")
    endif()
    add_subdirectory(runtime)
else()
    message(STATUS "P2EP_IMAGE not set: building tools and tests only")
endif()
