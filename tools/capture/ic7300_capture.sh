#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Records a WEFAX corpus file from an Icom transceiver: CAT via Hamlib + USB audio
# (spec TST-001). Receive-only (see rig_safe.sh); the rig state is restored on exit.
#
# Usage: ic7300_capture.sh <assigned_kHz> <minutes> [label]
#   e.g. ic7300_capture.sh 3855 60 DDH3
# Environment: AUDIO_DEV, RATE, PREAMP, USB_AF, OUT_DIR, RIG_* (see rig_safe.sh)

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=rig_safe.sh
source "$HERE/rig_safe.sh"

if [[ $# -lt 2 ]]; then
  sed -n '2,/^set -euo/p' "$0" | sed -n 's/^# \{0,1\}//p'
  exit 2
fi

KHZ=$1
MINUTES=$2
LABEL=${3:-wefax}
AUDIO_DEV=${AUDIO_DEV:-plughw:CARD=Device,DEV=0}
RATE=${RATE:-48000}
OUT_DIR=${OUT_DIR:-$HOME/owfax-corpus}
mkdir -p "$OUT_DIR"
BASE="$OUT_DIR/$(date -u +%Y%m%dT%H%MZ)_${LABEL}_${KHZ}kHz"

cleanup() {
  [[ -n ${SMETER_PID:-} ]] && kill "$SMETER_PID" 2>/dev/null || true
  rig_restore || true
  rig_stop || true
}
trap cleanup EXIT

rig_start
rig_save
PROFILE=$(rig_rx_profile "$KHZ")
echo "$PROFILE"

cat >"$BASE.json" <<EOF
{
  "label": "$LABEL",
  "assigned_khz": $KHZ,
  "rig_settings": "$PROFILE",
  "start_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "duration_s": $((MINUTES * 60)),
  "sample_rate": $RATE,
  "receiver": "Icom IC-7300MK2 (Hamlib $RIG_MODEL, USB audio)",
  "audio_device": "$AUDIO_DEV",
  "host": "$(hostname)"
}
EOF

# S-meter log every 10 s (spec CAT-005).
(
  echo "utc,strength_db"
  while sleep 10; do echo "$(date -u +%H:%M:%S),$(rig l STRENGTH 2>/dev/null || echo NA)"; done
) >"$BASE.smeter.csv" &
SMETER_PID=$!

echo "Recording $MINUTES min to $BASE.wav ..."
arecord -q -D "$AUDIO_DEV" -f S16_LE -r "$RATE" -c 1 -d $((MINUTES * 60)) "$BASE.wav"
echo "Done: $BASE.wav"
