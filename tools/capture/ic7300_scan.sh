#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Ranks WEFAX frequencies: tunes each one, records a short excerpt, scores it with
# `owfax-cli probe` and the S-meter. Receive-only (see rig_safe.sh).
#
# Usage: ic7300_scan.sh [seconds_per_freq] [kHz ...]
# Default list: DWD Pinneberg and Northwood (GYA).

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=rig_safe.sh
source "$HERE/rig_safe.sh"

DWELL=${1:-30}
shift || true
FREQS=("$@")
[[ ${#FREQS[@]} -gt 0 ]] || FREQS=(3855 7880 13882.5 2618.5 4610 8040 11086.5)
AUDIO_DEV=${AUDIO_DEV:-plughw:CARD=Device,DEV=0}
CLI=${OWFAX_CLI:-$HERE/../../build/apps/cli/owfax-cli}
TMP=$(mktemp -d)

cleanup() { rig_restore || true; rig_stop || true; rm -rf "$TMP"; }
trap cleanup EXIT

rig_start
rig_save

printf "%-9s %-8s %s\n" "kHz" "S(dB)" "probe"
for khz in "${FREQS[@]}"; do
  rig_rx_profile "$khz" >/dev/null
  sleep 2
  arecord -q -D "$AUDIO_DEV" -f S16_LE -r 48000 -c 1 -d "$DWELL" "$TMP/$khz.wav"
  s=$(rig l STRENGTH 2>/dev/null || echo NA)
  printf "%-9s %-8s %s\n" "$khz" "$s" "$("$CLI" probe "$TMP/$khz.wav")"
done
