# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Web showcase (docs/VITRINE-WEB.md): chart packages for the WordPress replay player.
# No third-party library; image and audio encoding are left to the packaging tool.

add_library(owfax_vitrine
  src/catalog.cpp
  src/geo.cpp
  src/package.cpp
  src/telemetry.cpp)
add_library(owfax::vitrine ALIAS owfax_vitrine)
target_include_directories(owfax_vitrine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(owfax_vitrine PUBLIC owfax::core owfax::net)
owfax_set_target_options(owfax_vitrine)
