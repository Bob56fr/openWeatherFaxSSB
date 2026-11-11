# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Dependency-free test runner (works offline and on every target, including qemu).

add_executable(owfax_tests
  test_main.cpp
  test_build_info.cpp
  test_capabilities.cpp
  test_corpus.cpp
  test_dsp.cpp
  test_engine.cpp
  test_files.cpp
  test_net.cpp
  test_settings.cpp
  test_wefax_decode.cpp
  test_stream_decoder.cpp
  test_wefax_params.cpp)
target_link_libraries(owfax_tests PRIVATE owfax::core owfax::io owfax::net)
owfax_set_target_options(owfax_tests)
target_compile_definitions(owfax_tests PRIVATE
  OWFAX_CORPUS_TSV="${CMAKE_CURRENT_SOURCE_DIR}/corpus/corpus.tsv")

add_test(NAME owfax_tests COMMAND owfax_tests)
set_tests_properties(owfax_tests PROPERTIES TIMEOUT 600)
