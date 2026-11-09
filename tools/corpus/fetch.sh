#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Downloads the real-signal test corpus listed in tests/corpus/corpus.tsv from the
# GitHub release "corpus-v1" and checks every SHA-256.
#
# Usage: fetch.sh [dest_dir]     (default: tests/corpus/data, ignored by git)
# Then:  OWFAX_CORPUS_DIR=<dest_dir> ctest --test-dir build -R corpus

set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:-$ROOT/tests/corpus/data}
TAG=${OWFAX_CORPUS_TAG:-corpus-v1}
BASE=${OWFAX_CORPUS_URL:-https://github.com/Bob56fr/openWeatherFaxSSB/releases/download/$TAG}
mkdir -p "$DEST"

tail -n +2 "$ROOT/tests/corpus/corpus.tsv" | while IFS=$'\t' read -r file sha _; do
  [[ -n $file ]] || continue
  if [[ -f $DEST/$file ]] && echo "$sha  $DEST/$file" | sha256sum -c --quiet - 2>/dev/null; then
    echo "ok      $file"
    continue
  fi
  echo "fetch   $file"
  curl -fsSL -o "$DEST/$file" "$BASE/$file"
  echo "$sha  $DEST/$file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
done
echo "Corpus ready in $DEST"
