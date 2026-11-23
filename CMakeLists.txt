cmake_minimum_required(VERSION 3.28)
project(G6Recomp VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "G6Recomp builds for Windows x64 only. Linux runs the Windows build under Proton.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(G6_NOGAME "Build without the user's game files: tools, libraries, and nogame tests only" OFF)
option(G6_BUILD_TESTS "Build the test suite" ON)
set(G6_GAME_DIR "${CMAKE_SOURCE_DIR}/private/game" CACHE PATH
    "Root of the user's decrypted game install (the folder holding the main executable)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(G6TargetDefaults)
include(G6GameFiles)
g6_check_game_files()

add_subdirectory(thirdparty EXCLUDE_FROM_ALL)
add_subdirectory(common)
add_subdirectory(tools/x64recomp)
add_subdirectory(tools/shaderrecomp)
add_subdirectory(tools/rpfscan)
add_subdirectory(runtime)

if(G6_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
