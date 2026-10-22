cmake_minimum_required(VERSION 3.16)
project(idRegistryServer VERSION 0.2.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

set(SOURCES
    src/main.cpp
    src/platform.cpp
    src/platform.h
)

add_executable(idRegistryServer
    ${SOURCES}
)

target_include_directories(idRegistryServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/httplib
)

target_link_libraries(idRegistryServer PRIVATE
    idRegistryDB
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(idRegistryServer PRIVATE ws2_32 crypt32 advapi32 shell32)
endif()

target_compile_options(idRegistryServer PRIVATE
    -Wall -Wextra
)

# Custom target to run the server
add_custom_target(run_server
    COMMAND "$<TARGET_FILE:idRegistryServer>"
    DEPENDS idRegistryServer
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running idRegistryServer"
)
