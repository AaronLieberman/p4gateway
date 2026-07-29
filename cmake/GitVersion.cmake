# Derives gw's build stamp from git and writes the generated version.cpp.
#
# Run in script mode (`cmake -P`), both at configure time and as a custom
# command on every build, so that committing restamps the binary without a
# reconfigure. The write goes through configure_file, which leaves the output
# untouched when nothing changed - so an unchanged HEAD costs a few git calls
# and no recompile.
#
# Nobody hand-edits a version here: the only number a human sets is the
# MAJOR.MINOR in the top-level project() call, passed in as GW_VERSION_BASE.
# The patch number is the commit count behind HEAD (main is linear, so that
# count is monotonic), and the commit sha is what actually ties a binary back
# to its source.
#
# Required -D arguments:
#   GW_VERSION_BASE  MAJOR.MINOR from project(), e.g. "1.0"
#   GW_SRC_DIR       the repo to interrogate (the project source dir)
#   GW_IN            path to cmake/version.cpp.in
#   GW_OUT           path of the version.cpp to write

foreach(required GW_VERSION_BASE GW_SRC_DIR GW_IN GW_OUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "GitVersion.cmake: -D ${required}=... is required")
    endif()
endforeach()

# Every git-derived field is best-effort: building from a source copy with no
# git, or no .git directory, must still produce a working binary - it just
# reports an unknown provenance.
set(GW_VERSION_COMMITS "")
set(GW_VERSION_COMMIT "")
set(GW_VERSION_DATE "")
set(GW_VERSION_BRANCH "")
set(GW_VERSION_DIRTY "false")
set(GW_VERSION_SHALLOW "false")

find_package(Git QUIET)

# Run a git command against the source repo; on any failure the result is the
# empty string, which every caller below treats as "unknown".
function(gw_git out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${GW_SRC_DIR}"
        OUTPUT_VARIABLE output
        RESULT_VARIABLE status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(status EQUAL 0)
        set(${out_var} "${output}" PARENT_SCOPE)
    endif()
endfunction()

gw_git(GW_VERSION_COMMIT rev-parse HEAD)
if(GW_VERSION_COMMIT)
    gw_git(GW_VERSION_DATE log -1 --format=%cI)

    gw_git(branch rev-parse --abbrev-ref HEAD)
    # A detached HEAD - a CI checkout of a PR merge commit, say - answers
    # literally "HEAD"; report no branch rather than that.
    if(NOT branch STREQUAL "HEAD")
        set(GW_VERSION_BRANCH "${branch}")
    endif()

    # A shallow clone (actions/checkout's default fetch-depth: 1) can count
    # only the commits it has, which would silently understate the version.
    # Record the shallowness instead and let the version string say so.
    gw_git(shallow rev-parse --is-shallow-repository)
    if(shallow STREQUAL "true")
        set(GW_VERSION_SHALLOW "true")
    else()
        gw_git(GW_VERSION_COMMITS rev-list --count HEAD)
    endif()

    # Tracked-file changes only: untracked scratch files and build directories
    # do not make a build "modified".
    gw_git(modified status --porcelain --untracked-files=no)
    if(modified)
        set(GW_VERSION_DIRTY "true")
    endif()
endif()

configure_file("${GW_IN}" "${GW_OUT}" @ONLY)
