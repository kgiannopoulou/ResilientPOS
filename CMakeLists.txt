cmake_minimum_required(VERSION 3.20)

project(ResilientPOS
    VERSION 0.1.0
    DESCRIPTION "Offline-first POS engine that keeps selling through network failures"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put executables at the top of the build dir so ./build/resilient-pos works
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR})
endforeach()

add_library(resilientpos_warnings INTERFACE)
if(MSVC)
    target_compile_options(resilientpos_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(resilientpos_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# --- Applications ------------------------------------------------------------
add_executable(resilient-pos apps/pos/main.cpp)
target_link_libraries(resilient-pos PRIVATE resilientpos_warnings)

add_executable(resilient-server apps/server/main.cpp)
target_link_libraries(resilient-server PRIVATE resilientpos_warnings)
