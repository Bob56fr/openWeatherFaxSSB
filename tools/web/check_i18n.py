#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""Checks the web interface translations (spec UI-005): every language file has the
same keys, and every key used by index.html / app.js is defined."""

import json
import pathlib
import re
import sys

WEB = pathlib.Path(__file__).resolve().parents[2] / "web"


def main() -> int:
    langs = {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in (WEB / "i18n").glob("*.json")}
    if not langs:
        print("no translation file")
        return 1
    errors = []
    reference = set(langs.get("fr", next(iter(langs.values()))))
    for name, table in sorted(langs.items()):
        for k in sorted(reference - set(table)):
            errors.append(f"{name}.json: missing {k}")
        for k in sorted(set(table) - reference):
            errors.append(f"{name}.json: extra {k}")
    src = (WEB / "app.js").read_text(encoding="utf-8") + (WEB / "index.html").read_text(encoding="utf-8")
    used = set(re.findall(r"t\('([a-z_]+\.[a-z_]+)'", src)) | set(re.findall(r'data-i18n="([a-z_.]+)"', src))
    for k in sorted(used - reference):
        errors.append(f"used but not translated: {k}")
    for e in errors:
        print(e)
    print(f"{len(langs)} languages, {len(reference)} keys, {len(used)} used directly: "
          + ("OK" if not errors else f"{len(errors)} error(s)"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
