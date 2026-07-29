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
# The patch number counts the commits since that MAJOR.MINOR was set, so
# bumping it in CMakeLists.txt resets the patch to 0 all by itself (main is
# linear, so the count climbs monotonically from there). The commit sha is
# what actually ties a binary back to its source.
#
# Required -D arguments:
#   GW_VERSION_BASE  MAJOR.MINOR from project(), e.g. "1.0"
#   GW_SRC_DIR       the repo to interrogate (the project source dir)
#   GW_IN            path to cmake/version.cpp.in
#   GW_OUT           path of the version.cpp to write
# Optional:
#   GW_WARN_UNANCHORED  warn when the MAJOR.MINOR bump commit can't be found
#                       (set only for the configure-time run, so the warning
#                       appears once rather than on every build)

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
        # Where does this MAJOR.MINOR start? The bump commit is the oldest one
        # that changed how often "VERSION <base>." occurs in CMakeLists.txt -
        # i.e. the commit that first wrote this MAJOR.MINOR into project().
        # -S takes a fixed string, so no regex dialect is involved, and the
        # trailing dot keeps a base of "3.2" from matching a
        # cmake_minimum_required of "VERSION 3.25".
        gw_git(bumps log --format=%H -S "VERSION ${GW_VERSION_BASE}."
               -- CMakeLists.txt)
        if(bumps)
            string(REPLACE "\n" ";" bumps "${bumps}")
            list(POP_BACK bumps bump)  # git log is newest-first; take the first
            gw_git(GW_VERSION_COMMITS rev-list --count "${bump}..HEAD")
        else()
            # No such commit: an unusual project() spelling, or a history that
            # never contained the bump. Counting the whole history keeps the
            # number monotonic, it just no longer resets at the bump.
            gw_git(GW_VERSION_COMMITS rev-list --count HEAD)
            if(GW_WARN_UNANCHORED)
                message(WARNING
                    "GitVersion: no commit in CMakeLists.txt's history "
                    "introduces \"VERSION ${GW_VERSION_BASE}.\", so the patch "
                    "number counts the whole history instead of resetting at "
                    "the ${GW_VERSION_BASE} bump. Spell the project() version "
                    "with all three components (VERSION ${GW_VERSION_BASE}.0) "
                    "and commit that bump on its own.")
            endif()
        endif()
    endif()

    # Tracked-file changes only: untracked scratch files and build directories
    # do not make a build "modified".
    gw_git(modified status --porcelain --untracked-files=no)
    if(modified)
        set(GW_VERSION_DIRTY "true")
    endif()
endif()

configure_file("${GW_IN}" "${GW_OUT}" @ONLY)
