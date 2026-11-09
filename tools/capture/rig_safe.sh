# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Receive-only Hamlib helpers, sourced by the capture scripts (spec NFR-001, CAT-002).
#
# - rig: whitelist wrapper around rigctl. Allowed: reads (f m l u t), F, M,
#   L AGC|PREAMP|ATT|USB_AF, U NB|NR|ANF, and "T 0" (force receive) only.
# - rig_start / rig_stop: private rigctld (serial port opened once, no PTT line).
# - rig_save / rig_restore: snapshot and restore of everything this tool may change.
# - rig_rx_profile: settings for WEFAX reception.

RIG_MODEL=${RIG_MODEL:-3094}  # IC-7300MK2 (Hamlib >= 4.7)
RIG_PORT=${RIG_PORT:-/dev/serial/by-id/usb-Icom_Inc._IC-7300MK2_IC-7300MK2_13003336-if00}
RIG_BAUD=${RIG_BAUD:-115200}
RIGCTLD_PORT=${RIGCTLD_PORT:-4533}
RIG_STATE_FILE=${RIG_STATE_FILE:-/tmp/owfax-rig-state.$$}

# HARDWARE PROTECTION (spec CAT-009): the IC-7300 switches its band-pass filters with
# mechanical relays. Back-and-forth band changes in quick succession can damage them.
# At least 5 s must separate two frequency changes. The timestamp is shared through a
# file so the rule also holds across scripts (restore, then the next capture).
RIG_BAND_GUARD_S=${RIG_BAND_GUARD_S:-6}
RIG_BAND_STAMP=${RIG_BAND_STAMP:-/tmp/owfax-last-freq-change}
if awk -v g="$RIG_BAND_GUARD_S" 'BEGIN { exit !(g < 5) }'; then
  echo "RIG_BAND_GUARD_S=$RIG_BAND_GUARD_S refused: minimum 5 s (relay protection)." >&2
  RIG_BAND_GUARD_S=5
fi

rig_band_guard() {
  local now last wait
  now=$(date +%s.%N)
  last=$(cat "$RIG_BAND_STAMP" 2>/dev/null || echo 0)
  wait=$(awk -v n="$now" -v l="$last" -v g="$RIG_BAND_GUARD_S" \
    'BEGIN { w = l + g - n; if (w > 0) printf "%.2f", w; else print 0 }')
  if [[ $wait != 0 ]]; then
    echo "Band-change guard: waiting ${wait} s (IC-7300 relays)" >&2
    sleep "$wait"
  fi
}

rig_band_mark() { date +%s.%N >"$RIG_BAND_STAMP"; }

# Talks to rigctld with its extended protocol ("+cmd" → header, "Key: value" lines,
# "RPRT n"). Prints the values, one per line; fails if RPRT != 0. Uses a raw TCP
# socket (fd 9 write, fd 8 read) rather than the rigctl client, whose NET model differs between
# Hamlib versions.
rig() {
  case "$1" in
    f|m|l|u|t|F|M) ;;
    L) [[ $2 =~ ^(AGC|PREAMP|ATT|USB_AF)$ ]] || { echo "refused: L $2" >&2; return 1; } ;;
    U) [[ $2 =~ ^(NB|NR|ANF)$ ]] || { echo "refused: U $2" >&2; return 1; } ;;
    T) [[ $# -eq 2 && $2 == 0 ]] || { echo "refused: T $*" >&2; return 1; } ;;
    *) echo "refused rig command: $1" >&2; return 1 ;;
  esac
  if [[ $1 == F ]]; then
    rig_band_guard
    rig_band_mark  # stamped before sending: a failed or slow reply still counts
  fi
  printf '+%s\n' "$*" >&9 || return 1
  local line first=1 rc=1
  while IFS= read -r -t 5 line <&8; do
    line=${line%$'\r'}
    if [[ $line =~ ^RPRT\ (-?[0-9]+) ]]; then
      rc=${BASH_REMATCH[1]}
      break
    fi
    if ((first)); then first=0; continue; fi  # header line, e.g. "get_freq:"
    if [[ $line == *": "* ]]; then echo "${line##*: }"; else echo "$line"; fi
  done
  [[ $rc == 0 ]]
}

rig_connect() {
  exec 9<>"/dev/tcp/127.0.0.1/$RIGCTLD_PORT"
  exec 8<&9  # replies are read from fd 8 (same socket)
}

rig_start() {
  if pgrep -x rigctld >/dev/null; then
    echo "Another rigctld is running: refusing to share the serial port." >&2
    return 1
  fi
  # -P RIG: PTT state is *read* over CI-V; keying stays impossible (whitelist above).
  rigctld -m "$RIG_MODEL" -r "$RIG_PORT" -s "$RIG_BAUD" -T 127.0.0.1 -t "$RIGCTLD_PORT" \
    -P RIG -C dtr_state=OFF,rts_state=OFF >/tmp/owfax-rigctld.log 2>&1 &
  RIGCTLD_PID=$!
  for _ in $(seq 1 20); do
    sleep 0.5
    rig_connect 2>/dev/null && rig f >/dev/null 2>&1 && break
  done
  # Opening a serial port raises DTR/RTS: make sure the radio is in receive.
  local ptt
  ptt=$(rig t 2>/dev/null || echo "?")
  if [[ $ptt != 0 ]]; then
    echo "PTT reads '$ptt' after opening the port: forcing receive." >&2
    rig T 0 || true
    [[ $(rig t 2>/dev/null) == 0 ]] || { echo "Cannot confirm receive state." >&2; rig_stop; return 1; }
  fi
  # Another application may have changed band just before we took the radio:
  # count our start as a change so the first F waits the guard delay.
  [[ -f $RIG_BAND_STAMP ]] || rig_band_mark
  echo "rigctld started (pid $RIGCTLD_PID), radio confirmed in receive."
}

rig_stop() {
  exec 8<&- 9>&- 2>/dev/null || true
  [[ -n ${RIGCTLD_PID:-} ]] && kill "$RIGCTLD_PID" 2>/dev/null && wait "$RIGCTLD_PID" 2>/dev/null
  RIGCTLD_PID=
}

rig_save() {
  {
    echo "FREQ=$(rig f)"
    mapfile -t mode < <(rig m)
    echo "MODE=${mode[0]}"
    echo "PASSBAND=${mode[1]:-0}"
    for l in AGC PREAMP ATT USB_AF; do echo "$l=$(rig l $l 2>/dev/null)"; done
    for u in NB NR ANF; do echo "F_$u=$(rig u $u 2>/dev/null)"; done
  } >"$RIG_STATE_FILE"
  echo "Saved rig state: $(tr '\n' ' ' <"$RIG_STATE_FILE")"
}

rig_restore() {
  [[ -f $RIG_STATE_FILE ]] || return 0
  # shellcheck disable=SC1090
  source "$RIG_STATE_FILE"
  rig F "$FREQ" || true
  rig M "$MODE" "$PASSBAND" || true
  for l in AGC PREAMP ATT USB_AF; do
    [[ -n ${!l} ]] && { rig L "$l" "${!l}" || true; }
  done
  for u in NB NR ANF; do
    local v="F_$u"
    [[ -n ${!v} ]] && { rig U "$u" "${!v}" || true; }
  done
  echo "Restored rig state."
  rm -f "$RIG_STATE_FILE"
}

# rig_tune <assigned_kHz>: USB-D dial = assigned − 1.9 kHz (spec CAT-003), WEFAX RX settings.
rig_rx_profile() {
  local khz=$1
  local dial
  dial=$(awk -v k="$khz" 'BEGIN { printf "%d", k * 1000 - 1900 }')
  rig F "$dial"
  rig M PKTUSB "${PASSBAND_HZ:-3000}"
  rig L AGC 3                      # slow
  rig U NB 0                       # blanker distorts FM transitions
  rig U NR 0                       # DSP noise reduction smears tones
  rig U ANF 0                      # auto-notch would erase the sub-carrier
  rig L ATT 0
  # Preamp off below 5 MHz (band noise dominates), PRE1 above.
  local pre=${PREAMP:-$(awk -v k="$khz" 'BEGIN { print (k < 5000 ? 0 : 1) }')}
  rig L PREAMP "$pre"
  # 0.15 ≈ −18 dBFS with AGC slow on the IC-7300MK2, no clipping (measured 2026-11-09).
  rig L USB_AF "${USB_AF:-0.15}"
  echo "Tuned ${khz} kHz (dial ${dial} Hz PKTUSB), AGC slow, NB/NR/ANF off, ATT 0, PREAMP ${pre}"
}
