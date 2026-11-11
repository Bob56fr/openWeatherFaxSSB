# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Local HTTP API and WebSocket live stream (spec ARC-002, ARC-005, DSP-007), kept out
# of the dependency-free core. No third-party library: builds on every target,
# Windows 7 and MinGW included.

find_package(Threads REQUIRED)
add_library(owfax_net
  src/api.cpp
  src/encoding.cpp
  src/crypto.cpp
  src/http_server.cpp
  src/json.cpp
  src/settings.cpp
  src/socket.cpp)
add_library(owfax::net ALIAS owfax_net)
target_include_directories(owfax_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(owfax_net PUBLIC owfax::core Threads::Threads)
if(WIN32)
  target_link_libraries(owfax_net PUBLIC ws2_32 advapi32)
endif()
owfax_set_target_options(owfax_net)
