cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME industrial-edge-gateway)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/User/*.c
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${USER_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    User/app
    User/bsp
    User/modbus
    User/net
    User/storage
    User/gui
    User/gui/ui
    User/gui/porting
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# ---- 云端凭据注入：本地 .env > cloud.h 占位默认 ----
# .env 已被 .gitignore 忽略，模板见 .env.example。每行 KEY=VALUE（取第一个
# '=' 之后的全部）注入为编译宏：字符串键带引号，CLOUD_TARGET 按标识符（不带引号）。
set(CLOUD_ENV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.env")
if(EXISTS "${CLOUD_ENV_FILE}")
    file(STRINGS "${CLOUD_ENV_FILE}" CLOUD_ENV_LINES)
    foreach(_line IN LISTS CLOUD_ENV_LINES)
        string(REPLACE "\r" "" _line "${_line}")
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        string(FIND "${_line}" "=" _eq)
        if(_eq LESS 1)
            # 非 KEY=VALUE 行（注释碎片/空行残余）静默跳过；file(STRINGS)
            # 会把非 ASCII 注释行打碎，不再告警免得噪声
            continue()
        endif()
        string(SUBSTRING "${_line}" 0 "${_eq}" _key)
        math(EXPR _vstart "${_eq} + 1")
        string(SUBSTRING "${_line}" "${_vstart}" -1 _val)
        string(STRIP "${_val}" _val)
        string(REPLACE ";" "\\;" _val "${_val}")   # 值里的分号会破坏 CMake 列表，转义
        if(_key STREQUAL "CLOUD_TARGET")
            target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "${_key}=${_val}")
        else()
            target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE "${_key}=\"${_val}\"")
        endif()
        message(STATUS "cloud 凭据: ${_key} <- .env")
    endforeach()
else()
    message(STATUS ".env 不存在：用 cloud.h 占位值（cp .env.example .env 填写）")
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
