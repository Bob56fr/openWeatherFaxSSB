# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors

add_executable(owfax-vitrine main.cpp)
target_link_libraries(owfax-vitrine PRIVATE owfax::vitrine)
owfax_set_target_options(owfax-vitrine)

install(TARGETS owfax-vitrine RUNTIME DESTINATION bin)
