# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Sets OWFAX_GIT_REVISION to the current commit (with "-dirty" suffix) or "unknown".
# Evaluated at configure time; re-run CMake to refresh it.

set(OWFAX_GIT_REVISION "unknown")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=12
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _owfax_git_rev
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _owfax_git_result)
  if(_owfax_git_result EQUAL 0 AND _owfax_git_rev)
    set(OWFAX_GIT_REVISION "${_owfax_git_rev}")
  endif()
endif()

message(STATUS "openWeatherFaxSSB ${PROJECT_VERSION} (git: ${OWFAX_GIT_REVISION})")
