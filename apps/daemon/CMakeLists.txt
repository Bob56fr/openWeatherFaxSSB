# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors

find_package(Threads REQUIRED)
add_executable(owfaxd main.cpp)
target_link_libraries(owfaxd PRIVATE owfax::io Threads::Threads)
owfax_set_target_options(owfaxd)

install(TARGETS owfaxd RUNTIME DESTINATION bin)
