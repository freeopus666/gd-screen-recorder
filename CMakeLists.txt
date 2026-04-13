cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(GDScreenRecorder VERSION 1.0.0)

# Geode/CMake PCH generation breaks on this machine because the user profile
# path contains Cyrillic characters. Disable PCH globally before Geode is added.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR
        "GEODE_SDK not defined! Set environment variable:\n"
        "  set GEODE_SDK=C:\\path\\to\\geode-sdk\n"
        "Install Geode SDK: https://docs.geode-sdk.org/getting-started/create-mod"
    )
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

target_link_libraries(${PROJECT_NAME} opengl32 ole32 mmdevapi Propsys)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /utf-8
        /diagnostics:classic
        /W0
    )
endif()

setup_geode_mod(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
