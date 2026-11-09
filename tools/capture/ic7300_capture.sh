#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Records a WEFAX corpus file from an Icom transceiver: CAT via Hamlib + USB audio.
# Builds the real-signal test corpus (spec TST-001).
#
# RECEIVE ONLY (spec NFR-001 / CAT-002): this script only ever sends the Hamlib
# commands f/m (read), F/M (set frequency and mode) and l STRENGTH (read S-meter).
# It never sends T (PTT) or any other command. The previous frequency and mode are
# restored on exit, including on Ctrl-C.
#
# Usage:
#   ic7300_capture.sh <assigned_kHz> <minutes> [label]
#   e.g. ic7300_capture.sh 3855 60 DDH3
#
# Environment:
#   RIG          Hamlib target: "net" (default, uses rigctld) or a serial device path
#   RIGCTLD      rigctld address (default 127.0.0.1:4532)
#   RIG_MODEL    Hamlib model for direct serial access (default 3094 = IC-7300MK2)
#   RIG_BAUD     serial speed (default 115200)
#   AUDIO_DEV    ALSA capture device (default plughw:CARD=Device,DEV=0)
#   RATE         sample rate (default 48000)
#   MODE         Hamlib mode (default PKTUSB = USB-D on Icom, bypasses TX/RX DSP coupling)
#   PASSBAND     filter width in Hz (default 3000)
#   OUT_DIR      output directory (default ~/owfax-corpus)

set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '2,/^set -euo/p' "$0" | sed -n 's/^# \{0,1\}//p'
  exit 2
fi

ASSIGNED_KHZ=$1
MINUTES=$2
LABEL=${3:-wefax}
RIG=${RIG:-net}
RIGCTLD=${RIGCTLD:-127.0.0.1:4532}
RIG_MODEL=${RIG_MODEL:-3094}
RIG_BAUD=${RIG_BAUD:-115200}
AUDIO_DEV=${AUDIO_DEV:-plughw:CARD=Device,DEV=0}
RATE=${RATE:-48000}
MODE=${MODE:-PKTUSB}
PASSBAND=${PASSBAND:-3000}
OUT_DIR=${OUT_DIR:-$HOME/owfax-corpus}

# Dial frequency in USB = assigned frequency - 1.9 kHz (spec CAT-003).
DIAL_HZ=$(awk -v k="$ASSIGNED_KHZ" 'BEGIN { printf "%d", k * 1000 - 1900 }')
SECONDS_TOTAL=$(( MINUTES * 60 ))

rig() {
  # Whitelist: refuse anything but read commands and F/M.
  case "$1" in
    f|m|F|M|l) ;;
    *) echo "refused rig command: $1" >&2; return 1 ;;
  esac
  if [[ $RIG == net ]]; then
    rigctl -m 2 -r "$RIGCTLD" "$@"
  else
    rigctl -m "$RIG_MODEL" -r "$RIG" -s "$RIG_BAUD" "$@"
  fi
}

mkdir -p "$OUT_DIR"
STAMP=$(date -u +%Y%m%dT%H%MZ)
BASE="$OUT_DIR/${STAMP}_${LABEL}_${ASSIGNED_KHZ}kHz"

PREV_FREQ=$(rig f)
mapfile -t PREV_MODE < <(rig m)
echo "Previous rig state: ${PREV_FREQ} Hz ${PREV_MODE[0]} ${PREV_MODE[1]:-}"

restore() {
  echo "Restoring ${PREV_FREQ} Hz ${PREV_MODE[0]} ${PREV_MODE[1]:-0}"
  rig F "$PREV_FREQ" || true
  rig M "${PREV_MODE[0]}" "${PREV_MODE[1]:-0}" || true
  [[ -n ${SMETER_PID:-} ]] && kill "$SMETER_PID" 2>/dev/null || true
}
trap restore EXIT

rig F "$DIAL_HZ"
rig M "$MODE" "$PASSBAND"
echo "Tuned: assigned ${ASSIGNED_KHZ} kHz -> dial $DIAL_HZ Hz $MODE ${PASSBAND} Hz"

cat > "$BASE.json" <<EOF
{
  "label": "$LABEL",
  "assigned_khz": $ASSIGNED_KHZ,
  "dial_hz": $DIAL_HZ,
  "mode": "$MODE",
  "passband_hz": $PASSBAND,
  "start_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "duration_s": $SECONDS_TOTAL,
  "sample_rate": $RATE,
  "receiver": "Icom IC-7300MK2 (CAT + USB audio)",
  "audio_device": "$AUDIO_DEV",
  "host": "$(hostname)"
}
EOF

# S-meter log every 10 s (spec CAT-005).
(
  echo "utc,strength_db"
  while sleep 10; do echo "$(date -u +%H:%M:%S),$(rig l STRENGTH 2>/dev/null || echo NA)"; done
) > "$BASE.smeter.csv" &
SMETER_PID=$!

echo "Recording $MINUTES min to $BASE.wav ..."
arecord -q -D "$AUDIO_DEV" -f S16_LE -r "$RATE" -c 1 -d "$SECONDS_TOTAL" "$BASE.wav"
echo "Done: $BASE.wav"
