# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors

add_executable(owfax-cli main.cpp)
target_link_libraries(owfax-cli PRIVATE owfax::core)
owfax_set_target_options(owfax-cli)

install(TARGETS owfax-cli RUNTIME DESTINATION bin)
