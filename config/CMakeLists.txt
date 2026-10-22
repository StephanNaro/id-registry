cmake_minimum_required(VERSION 3.16)
project(idRegistryConfig VERSION 0.2.0 LANGUAGES CXX)

option(BUILD_DEVELOPMENT_MODE "Show console window for debugging" OFF)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable automatic MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Source and header files
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/service.cpp
)

set(HEADERS
    src/mainwindow.h
    src/service.h
)

# Executable
qt_add_executable(idRegistryConfig
    ${SOURCES}
    ${HEADERS}
)

# Link libraries
target_link_libraries(idRegistryConfig PRIVATE
    idRegistryDB
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Include directories
target_include_directories(idRegistryConfig PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compiler flags
target_compile_options(idRegistryConfig PRIVATE
    -Wall -Wextra -Wno-ignored-attributes
)

# Hide console window on Windows (unless in development/debug mode)
if(WIN32)
    target_link_libraries(idRegistryConfig PRIVATE advapi32 shell32)
endif()

if(WIN32 AND NOT BUILD_DEVELOPMENT_MODE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(idRegistryConfig PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Custom target to run the executable
add_custom_target(run_config
    COMMAND "$<TARGET_FILE:idRegistryConfig>"
    DEPENDS idRegistryConfig
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running idRegistryConfig"
)
