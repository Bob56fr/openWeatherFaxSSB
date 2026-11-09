#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Checks the IC-7300 relay protection (spec CAT-009): two frequency changes are
# always at least 5 s apart, even across separate scripts, and a guard below 5 s
# is refused. No radio needed: the rigctld socket is replaced by a fake.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export RIG_BAND_STAMP
RIG_BAND_STAMP=$(mktemp)
rm -f "$RIG_BAND_STAMP"
fail() { echo "FAIL: $*" >&2; exit 1; }

# Fake rigctld: answers every extended command with "RPRT 0".
run_two_changes() {
  bash -c '
    source "$1/../rig_safe.sh"
    coproc FAKE { while read -r _; do printf "x:\nRPRT 0\n"; done; }
    exec 9>&"${FAKE[1]}" 8<&"${FAKE[0]}"
    t0=$(date +%s.%N)
    rig F 3853100 >/dev/null
    rig F 7878100 >/dev/null
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" "BEGIN { printf \"%.2f\", b - a }"
  ' _ "$HERE"
}

# 0. Without a previous stamp the first change is immediate; replies are received
#    (a 5 s read timeout would show up as > 5 s).
rm -f "$RIG_BAND_STAMP"
one=$(bash -c '
  source "$1/../rig_safe.sh"
  coproc FAKE { while read -r _; do printf "x:\nRPRT 0\n"; done; }
  exec 9>&"${FAKE[1]}" 8<&"${FAKE[0]}"
  t0=$(date +%s.%N); rig F 3853100 || exit 1; t1=$(date +%s.%N)
  awk -v a="$t0" -v b="$t1" "BEGIN { printf \"%.2f\", b - a }"
' _ "$HERE") || fail "fake rigctld reply not accepted"
awk -v e="$one" 'BEGIN { exit !(e < 1.0) }' || fail "single change took ${one} s"
echo "ok: fake rigctld answers, first change immediate (${one} s)"

# 1. Within one script (stamp left by step 0 is fresh, so both changes wait).
rm -f "$RIG_BAND_STAMP"
elapsed=$(RIG_BAND_GUARD_S=5 run_two_changes 2>/dev/null)
awk -v e="$elapsed" 'BEGIN { exit !(e >= 5.0 && e < 6.0) }' || fail "two changes ${elapsed} s apart (expected 5 to 6 s)"
echo "ok: same script, ${elapsed} s between changes"

# 2. Across scripts: the stamp left by the previous script is honoured.
start=$(date +%s.%N)
RIG_BAND_GUARD_S=5 run_two_changes >/dev/null 2>&1
end=$(date +%s.%N)
total=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.2f", b - a }')
awk -v e="$total" 'BEGIN { exit !(e >= 10.0 && e < 11.0) }' || fail "cross-script: ${total} s for two changes (expected 10 to 11 s)"
echo "ok: across scripts, first change waited (${total} s for two changes)"

# 3. A guard below 5 s is refused.
msg=$(RIG_BAND_GUARD_S=1 bash -c 'source "$1/../rig_safe.sh"; echo "$RIG_BAND_GUARD_S"' _ "$HERE" 2>&1)
[[ $msg == *"refused"* && ${msg##*$'\n'} == 5 ]] || fail "guard below 5 s accepted: $msg"
echo "ok: guard below 5 s refused"

rm -f "$RIG_BAND_STAMP"
echo "PASS"
