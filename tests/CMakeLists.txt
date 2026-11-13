# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Dependency-free test runner (works offline and on every target, including qemu).

add_executable(owfax_tests
  test_main.cpp
  test_build_info.cpp
  test_capabilities.cpp
  test_corpus.cpp
  test_disturbance.cpp
  test_dsp.cpp
  test_engine.cpp
  test_files.cpp
  test_net.cpp
  test_settings.cpp
  test_vitrine.cpp
  test_wefax_decode.cpp
  test_stream_decoder.cpp
  test_wefax_params.cpp)
target_link_libraries(owfax_tests PRIVATE owfax::core owfax::io owfax::net owfax::vitrine)
owfax_set_target_options(owfax_tests)
target_compile_definitions(owfax_tests PRIVATE
  OWFAX_CORPUS_TSV="${CMAKE_CURRENT_SOURCE_DIR}/corpus/corpus.tsv")

add_test(NAME owfax_tests COMMAND owfax_tests)
set_tests_properties(owfax_tests PROPERTIES TIMEOUT 600)

# Timing logic of the web showcase replay player (JavaScript), when Node.js is there.
find_program(OWFAX_NODE node)
if(OWFAX_NODE)
  add_test(NAME web_player COMMAND ${OWFAX_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/web/test_player.js)
endif()

# The WordPress plugin ships copies of the replay player and of the package schema.
set(OWFAX_WP ${PROJECT_SOURCE_DIR}/integrations/wordpress)
foreach(pair "web/vitrine/owfax-player.js|assets/owfax-player.js"
             "web/vitrine/owfax-player.css|assets/owfax-player.css"
             "docs/schemas/owfax-vitrine-0.1.schema.json|schema/owfax-vitrine-0.1.schema.json")
  string(REPLACE "|" ";" files "${pair}")
  list(GET files 0 original)
  list(GET files 1 copy)
  get_filename_component(name "${copy}" NAME)
  add_test(NAME wordpress_copy_${name}
           COMMAND ${CMAKE_COMMAND} -E compare_files ${PROJECT_SOURCE_DIR}/${original}
                   ${OWFAX_WP}/owfax-vitrine/${copy})
endforeach()

# Rules of the WordPress plugin (schema, file types, purge), when PHP is there.
find_program(OWFAX_PHP php)
if(OWFAX_PHP)
  add_test(NAME wordpress_rules COMMAND ${OWFAX_PHP} ${OWFAX_WP}/tests/test-rules.php)
endif()
