#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Turns a long capture into corpus entries (spec TST-001): one WAV per transmission,
# resampled to 11025 Hz, plus its reference decode (PNG) and a manifest row with the
# baseline metrics used by the regression test (TST-002).
#
# Usage: extract.sh <capture.wav> <out_dir> [shift_hz]
# The capture's <name>.json metadata (from ic7300_capture.sh) is read if present.
# Rows are appended to <out_dir>/corpus.tsv.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CLI=${OWFAX_CLI:-$HERE/../../build/apps/cli/owfax-cli}
IN=$1
OUT=$2
SHIFT=${3:-850}
RATE=11025
mkdir -p "$OUT"
TSV="$OUT/corpus.tsv"
[[ -f $TSV ]] || printf 'file\tsha256\tstation\tkhz\tstart_utc\tlines\tclock_ppm\ttuning_hz\tquality\tshift_hz\n' >"$TSV"

META="${IN%.wav}.json"
START_UTC=$(sed -n 's/.*"start_utc": "\(.*\)".*/\1/p' "$META" 2>/dev/null || true)
KHZ=$(sed -n 's/.*"assigned_khz": \([0-9.]*\).*/\1/p' "$META" 2>/dev/null || true)
LABEL=$(sed -n 's/.*"label": "\(.*\)".*/\1/p' "$META" 2>/dev/null || true)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$CLI" decode-all "$IN" "$TMP/seg" --shift "$SHIFT" | tee "$TMP/report.txt"

# One "Segment:" line per transmission, in order.
i=0
while read -r _ from to; do
  i=$((i + 1))
  # Absolute UTC start of the transmission.
  abs=$(date -u -d "$START_UTC + ${from%.*} seconds" +%Y%m%dT%H%MZ 2>/dev/null || echo "seg$i")
  name="${abs}_${LABEL:-wefax}_${KHZ:-0}kHz.wav"
  "$CLI" cut "$IN" "$OUT/$name" --from "$from" --to "$to" --rate "$RATE" >/dev/null
  "$CLI" decode "$OUT/$name" "$OUT/${name%.wav}.png" --shift "$SHIFT" >"$TMP/one.txt"
  # Enhanced version (validated enhancements) for the before/after comparison.
  "$CLI" decode "$OUT/$name" "$OUT/${name%.wav}_enh.png" --shift "$SHIFT" --enhance >/dev/null
  lines=$(sed -n 's/^Image: *[0-9]* x \([0-9]*\),.*/\1/p' "$TMP/one.txt")
  ppm=$(sed -n 's/^Clock error: *\([-+0-9.]*\) ppm/\1/p' "$TMP/one.txt")
  tune=$(sed -n 's/^Tuning offset: *\([-+0-9.]*\) Hz/\1/p' "$TMP/one.txt")
  qual=$(sed -n 's/.*quality \([0-9]*\) %.*/\1/p' "$TMP/one.txt")
  sha=$(sha256sum "$OUT/$name" | cut -d' ' -f1)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$sha" "${LABEL:-}" "${KHZ:-}" \
    "$abs" "$lines" "$ppm" "$tune" "$qual" "$SHIFT" >>"$TSV"
  echo "corpus: $name ($lines lines, ${ppm} ppm, ${tune} Hz, quality ${qual} %)"
done < <(grep '^Segment:' "$TMP/report.txt")
