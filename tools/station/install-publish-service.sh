#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Installs the publication to the web showcase as a systemd user service, without
# root (docs/VITRINE-WEB.md § 4.5, lot 5). Publication is off until this is done
# (VIT-003): nothing leaves the station otherwise.
#
# Usage: install-publish-service.sh --site <https://site> [options]
#   --site <url>          the WordPress site with the owfax-vitrine plugin
#   --queue <dir>         publication queue (default: ~/owfax-corpus/vitrine-queue)
#   --credentials <file>  "login:application password" of the owfax publisher
#                         account (default: ~/.config/owfax/vitrine-credentials)
#   --start               also start it now (by default it is only enabled)
#   --remove              stop, disable and remove the service (the queue is kept)
#
# Create the credentials file once, readable by you only:
#   umask 077; printf '%s:%s\n' owfax-station 'xxxx xxxx xxxx xxxx xxxx xxxx' \
#     > ~/.config/owfax/vitrine-credentials
# Packages enter the queue with: owfax-vitrine package ... --enqueue <queue>
# Run owfaxd with --publish-queue <queue> (install-user-service.sh --publish-queue)
# for the dashboard to show "publication active towards <site>".

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

SITE=""
QUEUE="$HOME/owfax-corpus/vitrine-queue"
CREDENTIALS="$HOME/.config/owfax/vitrine-credentials"
START=0
REMOVE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --site) SITE=$2; shift 2 ;;
    --queue) QUEUE=$2; shift 2 ;;
    --credentials) CREDENTIALS=$2; shift 2 ;;
    --start) START=1; shift ;;
    --remove) REMOVE=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -n 's/^# \{0,1\}//p'; exit 0 ;;
    *) echo "unknown option $1 (see --help)" >&2; exit 2 ;;
  esac
done

UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/owfax-publish.service"

if [[ $REMOVE == 1 ]]; then
  systemctl --user disable --now owfax-publish.service 2>/dev/null || true
  rm -f "$UNIT"
  systemctl --user daemon-reload
  echo "Publication service removed: nothing is sent any more (queue kept in $QUEUE)."
  exit 0
fi

BIN="$REPO/build/apps/vitrine/owfax-vitrine"
[[ -x $BIN ]] || { echo "build owfax-vitrine first: cmake --build build (missing $BIN)" >&2; exit 1; }
[[ $SITE == https://* ]] || { echo "--site: an https:// address is required" >&2; exit 2; }
command -v curl >/dev/null || { echo "curl is required: sudo apt install curl" >&2; exit 1; }
[[ -f $CREDENTIALS ]] || { echo "missing $CREDENTIALS (see --help)" >&2; exit 1; }
if [[ $(stat -c %a "$CREDENTIALS") != 600 && $(stat -c %a "$CREDENTIALS") != 400 ]]; then
  echo "$CREDENTIALS must be readable by you only: chmod 600 $CREDENTIALS" >&2
  exit 1
fi
mkdir -p "$QUEUE" "$UNIT_DIR"

esc() { printf '%s' "$1" | sed -e 's/[\\|&]/\\&/g'; }
sed -e "s|@BIN@|$(esc "$BIN")|g" -e "s|@SITE@|$(esc "$SITE")|g" \
    -e "s|@QUEUE@|$(esc "$QUEUE")|g" -e "s|@CREDENTIALS@|$(esc "$CREDENTIALS")|g" \
    "$HERE/owfax-publish.service.in" > "$UNIT"

if command -v systemd-analyze >/dev/null &&
   ! systemd-analyze --user verify "$UNIT" 2>&1 | sed "s|^|  |"; then
  echo "unit file rejected by systemd-analyze, not enabled: $UNIT" >&2
  exit 1
fi
systemctl --user daemon-reload
systemctl --user enable owfax-publish.service
if [[ $START == 1 ]]; then
  systemctl --user restart owfax-publish.service
  sleep 2
  systemctl --user --no-pager --lines=5 status owfax-publish.service || true
else
  echo "Enabled, not started. Start it with: systemctl --user start owfax-publish"
fi
echo "Journal: $QUEUE/journal.log   State: $BIN queue --queue $QUEUE"
