# Writes BuildInfo.h with the version, git branch and commit hash.
# Called at build time so the values always match the checked-out tree.
# Inputs: REPO_ROOT, VERSION, OUTPUT

find_package(Git QUIET)
set(GIT_BRANCH "unknown")
set(GIT_COMMIT "unknown")
set(GIT_DIRTY "")

if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${REPO_ROOT} OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
        WORKING_DIRECTORY ${REPO_ROOT} OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${REPO_ROOT} OUTPUT_VARIABLE GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(GIT_STATUS)
        set(GIT_DIRTY "-dirty")
    endif()
endif()

set(CONTENT "// Generated at build time by cmake/BuildInfo.cmake. Do not edit.
#pragma once
namespace kickemu::buildinfo
{
    inline constexpr const char* version = \"${VERSION}\";
    inline constexpr const char* gitBranch = \"${GIT_BRANCH}\";
    inline constexpr const char* gitCommit = \"${GIT_COMMIT}${GIT_DIRTY}\";
}
")

# Only touch the file when something changed, to avoid needless rebuilds.
set(OLD_CONTENT "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" OLD_CONTENT)
endif()
if(NOT "${OLD_CONTENT}" STREQUAL "${CONTENT}")
    file(WRITE "${OUTPUT}" "${CONTENT}")
endif()
