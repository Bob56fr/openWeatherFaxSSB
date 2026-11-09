# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors

# 3.18 = CMake shipped by Debian 11 (bullseye), kept as best-effort floor for
# Raspberry Pi OS Legacy. Supported floor is Debian 12 (spec §2.1).
cmake_minimum_required(VERSION 3.18)

project(openWeatherFaxSSB
  VERSION 0.1.0
  DESCRIPTION "Receive-only HF weather fax (WEFAX), RTTY and NAVTEX decoder"
  HOMEPAGE_URL "https://github.com/Bob56fr/openWeatherFaxSSB"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# AGPL-3.0 §13: the running program must point users to its exact source (spec UI-006).
set(OWFAX_SOURCE_URL "${PROJECT_HOMEPAGE_URL}" CACHE STRING "Public URL of the source repository")

option(OWFAX_BUILD_CLI "Build the owfax-cli command line tool" ON)
# PROJECT_IS_TOP_LEVEL needs CMake 3.21; compute it for 3.18.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(_owfax_top_level ON)
else()
  set(_owfax_top_level OFF)
endif()
option(OWFAX_BUILD_TESTS "Build unit tests" ${_owfax_top_level})
option(OWFAX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OwfaxCompilerOptions)
include(OwfaxGitInfo)

add_subdirectory(core)

if(OWFAX_BUILD_CLI)
  add_subdirectory(apps/cli)
endif()

if(OWFAX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
