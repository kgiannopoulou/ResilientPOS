cmake_minimum_required(VERSION 3.20)

project(ResilientPOS
    VERSION 0.1.0
    DESCRIPTION "Offline-first POS engine that keeps selling through network failures"
    LANGUAGES C CXX)

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

option(RESILIENTPOS_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

# --- SQLite (amalgamation, pinned and hash-checked) ----------------------------
FetchContent_Declare(
    sqlite3
    URL      https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# --- Core library ------------------------------------------------------------
add_library(resilientpos_core
    src/core/Money.cpp
    src/core/TransactionStatus.cpp
    src/core/Transaction.cpp
    src/util/UuidGenerator.cpp
    src/storage/SQLiteTransactionRepository.cpp
    src/network/SendResult.cpp
)
target_include_directories(resilientpos_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(resilientpos_core
    PUBLIC sqlite3
    PRIVATE resilientpos_warnings)
if(MSVC)
    target_compile_options(resilientpos_core PUBLIC /utf-8)
endif()

# --- Applications ------------------------------------------------------------
add_executable(resilient-pos apps/pos/main.cpp)
target_link_libraries(resilient-pos PRIVATE resilientpos_core resilientpos_warnings)

add_executable(resilient-server apps/server/main.cpp)
target_link_libraries(resilient-server PRIVATE resilientpos_core resilientpos_warnings)

# --- Tests -------------------------------------------------------------------
if(RESILIENTPOS_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    add_executable(resilientpos_tests
        tests/MoneyTests.cpp
        tests/TransactionStatusTests.cpp
        tests/UuidGeneratorTests.cpp
        tests/TransactionTests.cpp
        tests/RepositoryTests.cpp
        tests/SendResultTests.cpp
    )
    target_link_libraries(resilientpos_tests
        PRIVATE resilientpos_core resilientpos_warnings Catch2::Catch2WithMain)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(resilientpos_tests)
endif()
