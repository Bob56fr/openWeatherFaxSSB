#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Live reception with the owfaxd engine on an Icom transceiver: CAT via rig_safe.sh
# (receive only, >= 5 s between frequency changes, state restored), audio straight
# from the USB sound card through ALSA. Charts appear in <corpus>/decoded as they are
# received (live preview, then raw, enhanced and metadata), for the lab dashboard.
# Shares the radio lock with run_session.sh.
#
# Usage: owfaxd_live.sh <assigned_kHz> <minutes> <label> [shift_hz]
#   e.g. owfaxd_live.sh 13882.5 120 DWD 850
# Environment: AUDIO_DEV, RATE, OWFAX_CORPUS, OWFAXD, RIG_* (see rig_safe.sh),
#   HTTP_PORT (web interface and API, default 8080; 0 = none)

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=rig_safe.sh
source "$HERE/rig_safe.sh"

if [[ $# -lt 3 ]]; then
  sed -n '2,/^set -euo/p' "$0" | sed -n 's/^# \{0,1\}//p'
  exit 2
fi

KHZ=$1
MINUTES=$2
LABEL=$3
SHIFT=${4:-800}
AUDIO_DEV=${AUDIO_DEV:-plughw:CARD=Device,DEV=0}
RATE=${RATE:-48000}
CORPUS=${OWFAX_CORPUS:-$HOME/owfax-corpus}
OWFAXD=${OWFAXD:-$HERE/../../build/apps/daemon/owfaxd}
HTTP_PORT=${HTTP_PORT:-8080}
HTTP_ARGS=()
if [[ $HTTP_PORT != 0 ]]; then
  HTTP_ARGS=(--http "$HTTP_PORT" --web "$HERE/../../web" --catalog "$HERE/../../catalog/stations")
fi
J=$CORPUS/journal.log
mkdir -p "$CORPUS/decoded"

exec 7>/tmp/owfax-radio.lock
if ! flock -n 7; then
  echo "radio busy (another session holds /tmp/owfax-radio.lock)" >&2
  exit 1
fi

cleanup() {
  rig_restore || true
  rig_stop || true
}
trap cleanup EXIT

rig_start
rig_save
PROFILE=$(rig_rx_profile "$KHZ")
echo "$(date -u '+%Y-%m-%d %H:%MZ')  $LABEL : owfaxd en direct sur $KHZ kHz, $MINUTES min ($PROFILE)" >>"$J"

"$OWFAXD" --input "alsa:$AUDIO_DEV" --rate "$RATE" --out "$CORPUS/decoded" \
  --label "${LABEL}_${KHZ}kHz" --shift "$SHIFT" --duration $((MINUTES * 60)) --journal "$J" \
  "${HTTP_ARGS[@]}"
