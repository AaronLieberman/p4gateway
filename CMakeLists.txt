cmake_minimum_required(VERSION 3.25)

project(p4gateway
    VERSION 1.0.0
    DESCRIPTION "Git-to-Perforce gateway: work in Git, ship Perforce changelists"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # /MP compiles the TUs in each target in parallel; without it cl.exe
    # builds a target's files one at a time on a single core.
    add_compile_options(/W4 /permissive- /utf-8 /MP)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(p4gw_core STATIC
    src/subprocess.cpp
    src/config.cpp
    src/git.cpp
    src/p4.cpp
    src/p4ops.cpp
    src/mirror.cpp
    src/statusview.cpp
    src/shelf.cpp
    src/commands/setup.cpp
    src/commands/init.cpp
    src/commands/import.cpp
    src/commands/prepare.cpp
    src/commands/status.cpp
    src/commands/shelf.cpp
    src/commands/doctor.cpp
    src/commands/integtest.cpp
)
target_include_directories(p4gw_core PUBLIC src)

# Precompile the heavy, stable standard headers shared across the library's
# TUs. <filesystem>/<expected> dominate parse time and appear in most files.
# Only STL headers belong here - our own headers change too often to cache.
target_precompile_headers(p4gw_core PRIVATE
    <filesystem>
    <string>
    <vector>
    <expected>
    <sstream>
    <optional>
    <cstdio>
)

add_executable(gw src/main.cpp)
target_link_libraries(gw PRIVATE p4gw_core)

enable_testing()
add_subdirectory(tests)
