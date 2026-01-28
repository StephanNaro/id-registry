cmake_minimum_required(VERSION 3.16)
project(idRegistryGUI LANGUAGES CXX)

option(BUILD_DEVELOPMENT_MODE "Build in development mode" ON) # Default to development mode

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Find SQLite3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Enable automatic MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Source files
set(SOURCES
    src/database.cpp
    src/main.cpp
    src/mainwindow.cpp
)

# Header files (for MOC)
set(HEADERS
    include/database.h
    include/mainwindow.h
)

# UI files (add if you have .ui forms, e.g., for dialogs)
set(FORMS
    # e.g., src/somedialog.ui
    # Add paths to any .ui files here
)

# Resource files (add if you have .qrc for icons, etc.)
set(RESOURCES
    # e.g., resources.qrc
)

# Executable
add_executable(idRegistryGUI ${SOURCES} ${HEADERS} ${FORMS} ${RESOURCES})

# Add DEVELOPMENT_MODE definition and rename output for development
if(BUILD_DEVELOPMENT_MODE)
    target_compile_definitions(idRegistryGUI PRIVATE DEVELOPMENT_MODE)
    set_target_properties(idRegistryGUI PROPERTIES OUTPUT_NAME "idRegistryGUIDev")
else()
    set_target_properties(idRegistryGUI PROPERTIES OUTPUT_NAME "idRegistryGUI")
endif()

# Link libraries
target_link_libraries(idRegistryGUI PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    ${SQLITE3_LIBRARIES}
)

# Include directories
target_include_directories(idRegistryGUI PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SQLITE3_INCLUDE_DIRS}
)

# Compiler flags: Common flags plus build-specific optimizations
target_compile_options(idRegistryGUI PRIVATE
    -Wall -Wextra -Wno-ignored-attributes
)

# Build-specific compiler and linker options
if(BUILD_DEVELOPMENT_MODE)
    target_compile_options(idRegistryGUI PRIVATE -g)  # Debugging symbols
else()
    target_compile_options(idRegistryGUI PRIVATE -O2 -s)
    if(WIN32)
        target_link_options(idRegistryGUI PRIVATE -Wl,--strip-all)
    else()
        target_link_options(idRegistryGUI PRIVATE -s)
    endif()
endif()

# Windows-specific linker flag (only for release, to hide console window)
if(WIN32)
    if(NOT BUILD_DEVELOPMENT_MODE)
        target_link_options(idRegistryGUI PRIVATE -mwindows)
    endif()
endif()

# Custom targets to run the executables
add_custom_target(run_dev
    COMMAND "$<TARGET_FILE:idRegistryGUI>"
    DEPENDS idRegistryGUI
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running development executable"
)

add_custom_target(run_release
    COMMAND "$<TARGET_FILE:idRegistryGUI>"
    DEPENDS idRegistryGUI
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running release executable"
)