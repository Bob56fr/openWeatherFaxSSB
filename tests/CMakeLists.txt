# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Dependency-free test runner (works offline and on every target, including qemu).

add_executable(owfax_tests
  test_main.cpp
  test_wefax_params.cpp
  test_capabilities.cpp
  test_build_info.cpp)
target_link_libraries(owfax_tests PRIVATE owfax::core)
owfax_set_target_options(owfax_tests)

add_test(NAME owfax_tests COMMAND owfax_tests)
