#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Scans candidate frequencies, then records on the best one until a given UTC time.
# All rig access goes through rig_safe.sh (receive-only, >= 5 s between frequency
# changes, state restored after the scan and after the capture).
#
# Usage: scan_then_capture.sh <label> <until_HH:MM> <kHz> [kHz ...]
#   e.g. scan_then_capture.sh GYA 23:59 2618.5 4610 8040

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LABEL=$1
UNTIL=$2
shift 2
DWELL=${DWELL:-20}
J=${OWFAX_JOURNAL:-$HOME/owfax-corpus/journal.log}
log() { echo "$(date -u '+%Y-%m-%d %H:%MZ')  $*" | tee -a "$J"; }

log "$LABEL : balayage ${DWELL} s sur $* kHz"
SCAN=$("$HERE/ic7300_scan.sh" "$DWELL" "$@" 2>&1 | tee /dev/stderr)
# Rank by carrier presence and quality (in-band energy x FM consistency), NOT by the
# image score: stations send an idle carrier before a chart (DWD, 2026-11-10 04:24
# UTC: the scan ran 6 min before the 04:30 chart and the session was wrongly skipped).
# Noise alone gives ~0.25, a station (chart or idle carrier) ~0.8-1.
BEST=$(echo "$SCAN" | awk '/band=/ {
          b = 0; m = 0
          for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "band") b = kv[2]
            if (kv[1] == "fm_in_range") m = kv[2]
          }
          p = b * m
          if (p > best) { best = p; f = $1 }
        }
        END { if (f != "" && best >= 0.5) print f, best }')
if [[ -z $BEST ]]; then
  log "$LABEL : aucun signal exploitable, pas de capture"
  exit 1
fi
read -r KHZ SCORE <<<"$BEST"
now=$(date -u +%s)
end=$(date -u -d "$UNTIL" +%s)
MIN=$(((end - now) / 60))
if ((MIN < 2)); then
  log "$LABEL : plus assez de temps avant $UNTIL UTC"
  exit 1
fi
log "$LABEL : meilleure fréquence $KHZ kHz (score $SCORE), capture $MIN min"
exec "$HERE/ic7300_capture.sh" "$KHZ" "$MIN" "$LABEL"
