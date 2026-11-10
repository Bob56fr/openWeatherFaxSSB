# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp"
  @ONLY)

add_library(owfax_core
  src/audio/resample.cpp
  src/audio/source.cpp
  src/audio/wav.cpp
  src/capabilities.cpp
  src/dsp/fir.cpp
  src/dsp/fm_demod.cpp
  src/dsp/spectrum.cpp
  src/engine/receiver.cpp
  src/image/gray_image.cpp
  src/image/png_writer.cpp
  src/image/zlib.cpp
  src/wefax/decoder.cpp
  src/wefax/modulator.cpp
  src/wefax/probe.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp")
add_library(owfax::core ALIAS owfax_core)

target_include_directories(owfax_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(owfax_core PUBLIC cxx_std_17)
owfax_set_target_options(owfax_core)

set_target_properties(owfax_core PROPERTIES
  VERSION ${PROJECT_VERSION}
  POSITION_INDEPENDENT_CODE ON)  # linked into the Android JNI shared library
