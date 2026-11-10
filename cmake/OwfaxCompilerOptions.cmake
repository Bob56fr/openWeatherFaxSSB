# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# owfax_set_target_options(<target>)
# Applies the project-wide warning level and platform definitions to a target.

function(owfax_set_target_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    # Portable standard functions (getenv, fopen...) are flagged C4996 by MSVC only.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(OWFAX_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
    if(OWFAX_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(WIN32)
    # Windows 7 is the oldest best-effort target (spec §2.1): never use newer APIs.
    target_compile_definitions(${target} PRIVATE
      _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MINGW)
      # Self-contained binaries: no MinGW runtime DLLs to ship.
      target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    endif()
  endif()
endfunction()
