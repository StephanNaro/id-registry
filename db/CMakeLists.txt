cmake_minimum_required(VERSION 3.16)

# Shared database library used by both server and config
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)

add_library(idRegistryDB STATIC
    src/db.cpp
    src/db.h
)

target_include_directories(idRegistryDB PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(idRegistryDB PUBLIC
    SQLite3::SQLite3
)

target_compile_options(idRegistryDB PRIVATE
    -Wall -Wextra
)
