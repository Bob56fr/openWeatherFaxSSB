#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Unattended reception session (for cron): scan, capture on the best frequency until
# a UTC time, then extract every transmission into the corpus and publish the decoded
# charts to the lab dashboard. One session at a time (exclusive lock on the radio).
# All rig access goes through rig_safe.sh (receive only, >= 5 s between frequency
# changes, rig state restored).
#
# Usage: run_session.sh <label> <until_HH:MM> <shift_hz> <kHz> [kHz ...]
#   e.g. run_session.sh DWD 15:05 850 7880 13882.5 3855

set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LABEL=$1
UNTIL=$2
SHIFT=$3
shift 3
CORPUS=${OWFAX_CORPUS:-$HOME/owfax-corpus}
J=$CORPUS/journal.log
log() { echo "$(date -u '+%Y-%m-%d %H:%MZ')  $*" >>"$J"; }

exec 7>/tmp/owfax-radio.lock
if ! flock -w 900 7; then
  log "$LABEL : radio occupée, session annulée"
  exit 1
fi

log "$LABEL : session jusqu'à $UNTIL UTC ($* kHz)"
if ! "$HERE/scan_then_capture.sh" "$LABEL" "$UNTIL" "$@"; then
  log "$LABEL : pas de capture"
  exit 0
fi

wav=$(ls -t "$CORPUS"/*_"${LABEL}"_*kHz.wav 2>/dev/null | head -1)
[[ -n $wav ]] || { log "$LABEL : fichier de capture introuvable"; exit 1; }
log "$LABEL : extraction de $(basename "$wav")"
before=$(wc -l <"$CORPUS/corpus-v1/corpus.tsv" 2>/dev/null || echo 1)
nice "$HERE/../corpus/extract.sh" "$wav" "$CORPUS/corpus-v1" "$SHIFT" >"$CORPUS/extract.log" 2>&1
after=$(wc -l <"$CORPUS/corpus-v1/corpus.tsv" 2>/dev/null || echo 1)
mkdir -p "$CORPUS/decoded"
tail -n $((after - before)) "$CORPUS/corpus-v1/corpus.tsv" | while IFS=$'\t' read -r file _ _ _ _ lines ppm tune qual _; do
  cp -f "$CORPUS/corpus-v1/${file%.wav}.png" "$CORPUS/decoded/" 2>/dev/null
  log "$LABEL : carte ${file%.wav} — $lines lignes, $ppm ppm, $tune Hz, qualité $qual %"
done
log "$LABEL : $((after - before)) émission(s) ajoutée(s) au corpus"
