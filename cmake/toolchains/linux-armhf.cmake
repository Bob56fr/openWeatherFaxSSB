# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Cross-compilation for 32-bit ARM Linux (Raspberry Pi OS armhf, Pi Zero 2 W / Pi 3B).
# Debian/Ubuntu: apt install g++-arm-linux-gnueabihf qemu-user
# Tests run through qemu-arm when it is available.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(OWFAX_QEMU_ARM qemu-arm)
if(OWFAX_QEMU_ARM)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${OWFAX_QEMU_ARM};-L;/usr/arm-linux-gnueabihf")
endif()
