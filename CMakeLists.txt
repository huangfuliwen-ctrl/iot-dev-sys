cmake_minimum_required(VERSION 3.16)
project(dev-sys VERSION 1.0.0 LANGUAGES CXX C)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C standard for HAL/driver code
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find packages
find_package(Threads REQUIRED)

# External dependencies (can be overridden for embedded builds)
option(USE_SYSTEM_OPENSSL "Use system OpenSSL" ON)
option(USE_SYSTEM_CURL   "Use system libcurl"   ON)
option(USE_SYSTEM_SQLITE "Use system SQLite3"   ON)
option(BUILD_TESTS       "Build unit tests"      ON)

# Conditional compile definitions
if(USE_SYSTEM_OPENSSL)
    add_compile_definitions(HAS_OPENSSL)
endif()
if(USE_SYSTEM_CURL)
    add_compile_definitions(HAS_CURL)
endif()
if(USE_SYSTEM_SQLITE)
    add_compile_definitions(HAS_SQLITE USE_SYSTEM_SQLITE)
endif()

# Common include directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# Subdirectories
add_subdirectory(src)
add_subdirectory(config)

# Device Simulator (standalone tool, always built)
add_executable(device_simulator test/device_simulator.cpp)
target_include_directories(device_simulator PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(device_simulator PRIVATE Threads::Threads)
set_target_properties(device_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
