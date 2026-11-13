#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Installs owfaxd as a systemd user service (spec BG-005), without root.
#
# Usage: install-user-service.sh [options]
#   --input <uri>    audio input used until one is chosen in the web interface
#                    (default: alsa:plughw:CARD=Device,DEV=0, USB sound card of a TRX)
#   --port <n>       web interface and API port (default: 8080)
#   --charts <dir>   where charts are written (default: ~/owfax-corpus/decoded)
#   --vitrine <dir>  web showcase packages, replayed on the Vitrine page
#   --publish-queue <dir>  queue of owfax-vitrine send: the dashboard shows when the
#                    publication to the web showcase is on (install-publish-service.sh)
#   --start          also start it now (by default it is only enabled)
#   --remove         stop, disable and remove the service
#
# The settings and the skipper password live in ~/.config/owfax/owfaxd.json, outside
# the charts directory. To start at boot without anybody logged in, lingering must be
# enabled once by an administrator: sudo loginctl enable-linger "$USER".
#
# The service holds the sound card: stop it before a lab capture session that uses
# the same card (systemctl --user stop owfaxd), and start it again afterwards.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

INPUT="alsa:plughw:CARD=Device,DEV=0"
PORT=8080
CHARTS="$HOME/owfax-corpus/decoded"
VITRINE=""
PUBLISH_QUEUE=""
START=0
REMOVE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --input) INPUT=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --charts) CHARTS=$2; shift 2 ;;
    --vitrine) VITRINE=$2; shift 2 ;;
    --publish-queue) PUBLISH_QUEUE=$2; shift 2 ;;
    --start) START=1; shift ;;
    --remove) REMOVE=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -n 's/^# \{0,1\}//p'; exit 0 ;;
    *) echo "unknown option $1 (see --help)" >&2; exit 2 ;;
  esac
done

UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/owfaxd.service"

if [[ $REMOVE == 1 ]]; then
  systemctl --user disable --now owfaxd.service 2>/dev/null || true
  rm -f "$UNIT"
  systemctl --user daemon-reload
  echo "owfaxd service removed (settings kept in ~/.config/owfax)."
  exit 0
fi

OWFAXD="$REPO/build/apps/daemon/owfaxd"
[[ -x $OWFAXD ]] || { echo "build owfaxd first: cmake --build build (missing $OWFAXD)" >&2; exit 1; }
[[ $PORT =~ ^[0-9]+$ ]] || { echo "--port: a number" >&2; exit 2; }
CONFIG="$HOME/.config/owfax/owfaxd.json"
JOURNAL="$(dirname "$CHARTS")/journal.log"
mkdir -p "$CHARTS" "$(dirname "$CONFIG")" "$UNIT_DIR"
case "$(cd "$(dirname "$CONFIG")" && pwd)/" in
  "$(cd "$CHARTS" && pwd)/"*) echo "the settings file must stay outside the charts directory" >&2; exit 1 ;;
esac

EXTRA=""
[[ -n $VITRINE ]] && EXTRA+=" --vitrine \"$VITRINE\""
[[ -n $PUBLISH_QUEUE ]] && EXTRA+=" --publish-queue \"$PUBLISH_QUEUE\""

# Template → unit. Values are escaped for sed (|, & and \).
esc() { printf '%s' "$1" | sed -e 's/[\\|&]/\\&/g'; }
sed -e "s|@REPO@|$(esc "$REPO")|g" -e "s|@OWFAXD@|$(esc "$OWFAXD")|g" \
    -e "s|@CONFIG@|$(esc "$CONFIG")|g" -e "s|@INPUT@|$(esc "$INPUT")|g" \
    -e "s|@CHARTS@|$(esc "$CHARTS")|g" -e "s|@JOURNAL@|$(esc "$JOURNAL")|g" \
    -e "s|@EXTRA@|$(esc "$EXTRA")|g" -e "s|@PORT@|$PORT|g" "$HERE/owfaxd.service.in" > "$UNIT"

if command -v systemd-analyze >/dev/null &&
   ! systemd-analyze --user verify "$UNIT" 2>&1 | sed "s|^|  |"; then
  echo "unit file rejected by systemd-analyze, not enabled: $UNIT" >&2
  exit 1
fi
systemctl --user daemon-reload
systemctl --user enable owfaxd.service
if [[ $START == 1 ]]; then
  systemctl --user restart owfaxd.service
  sleep 2
  systemctl --user --no-pager --lines=5 status owfaxd.service || true
else
  echo "Enabled, not started. Start it with: systemctl --user start owfaxd"
fi
echo "Web interface: http://$(hostname -I 2>/dev/null | awk '{print $1}'):$PORT/"
if [[ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != "yes" ]]; then
  echo "To start at boot without logging in, run once: sudo loginctl enable-linger $USER"
fi
