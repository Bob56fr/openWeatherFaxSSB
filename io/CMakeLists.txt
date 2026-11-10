# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Hardware input back-ends, kept out of the dependency-free core.

add_library(owfax_io src/audio_input.cpp)
add_library(owfax::io ALIAS owfax_io)
target_include_directories(owfax_io PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(owfax_io PUBLIC owfax::core)
owfax_set_target_options(owfax_io)

# ALSA (Linux desktop and Raspberry Pi). AUTO: used when the headers are installed.
set(OWFAX_WITH_ALSA AUTO CACHE STRING "ALSA capture back-end: AUTO, ON or OFF")
set_property(CACHE OWFAX_WITH_ALSA PROPERTY STRINGS AUTO ON OFF)
set(OWFAX_HAVE_ALSA OFF)
if(NOT OWFAX_WITH_ALSA STREQUAL "OFF" AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  if(OWFAX_WITH_ALSA STREQUAL "ON")
    find_package(ALSA REQUIRED)
  else()
    find_package(ALSA QUIET)
  endif()
  if(ALSA_FOUND)
    set(OWFAX_HAVE_ALSA ON)
  endif()
elseif(OWFAX_WITH_ALSA STREQUAL "ON")
  message(FATAL_ERROR "OWFAX_WITH_ALSA=ON is only possible on Linux")
endif()
if(OWFAX_HAVE_ALSA)
  target_sources(owfax_io PRIVATE src/alsa_input.cpp)
  target_compile_definitions(owfax_io PRIVATE OWFAX_HAVE_ALSA=1)
  target_link_libraries(owfax_io PRIVATE ALSA::ALSA)
endif()
message(STATUS "owfax audio inputs: wav, stdin; ALSA: ${OWFAX_HAVE_ALSA}")
