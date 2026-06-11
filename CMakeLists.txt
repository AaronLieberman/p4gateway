cmake_minimum_required(VERSION 3.25)

project(p4gateway
    VERSION 0.1.0
    DESCRIPTION "Git-to-Perforce gateway: work in Git, ship Perforce changelists"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(p4gw_core STATIC
    src/process.cpp
    src/config.cpp
    src/git.cpp
    src/p4.cpp
    src/p4ops.cpp
    src/mirror.cpp
    src/commands/setup.cpp
    src/commands/init.cpp
    src/commands/import.cpp
    src/commands/prepare.cpp
    src/commands/status.cpp
    src/commands/doctor.cpp
)
target_include_directories(p4gw_core PUBLIC src)

add_executable(gw src/main.cpp)
target_link_libraries(gw PRIVATE p4gw_core)

enable_testing()
add_subdirectory(tests)
