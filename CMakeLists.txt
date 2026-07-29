cmake_minimum_required(VERSION 3.25)

# Only MAJOR.MINOR below is set by hand. The patch number and the commit sha
# that identifies a build are derived from git - see cmake/GitVersion.cmake and
# the gw_version target further down.
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

# The version stamp is regenerated from git on every build (a new commit must
# restamp the binary without a reconfigure), and once here so the file exists
# before the first compile. GitVersion.cmake writes through configure_file, so
# an unchanged HEAD leaves version.cpp byte-identical and nothing recompiles.
set(gw_version_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp)
set(gw_version_args
    -D GW_VERSION_BASE=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    -D GW_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -D GW_IN=${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cpp.in
    -D GW_OUT=${gw_version_cpp}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
)
execute_process(COMMAND ${CMAKE_COMMAND} ${gw_version_args} COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(gw_version
    BYPRODUCTS ${gw_version_cpp}
    COMMAND ${CMAKE_COMMAND} ${gw_version_args}
    COMMENT "Stamping gw version from git"
    VERBATIM
)
set_source_files_properties(${gw_version_cpp} PROPERTIES GENERATED TRUE)

add_library(p4gw_core STATIC
    ${gw_version_cpp}
    src/version.cpp
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
    src/commands/syncback.cpp
    src/commands/status.cpp
    src/commands/shelf.cpp
    src/commands/doctor.cpp
    src/commands/integtest.cpp
)
target_include_directories(p4gw_core PUBLIC src)
add_dependencies(p4gw_core gw_version)

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
