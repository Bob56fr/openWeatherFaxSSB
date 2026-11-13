#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
#
# Translations of the WordPress plugin (WPP-043), without gettext installed:
#
#   python3 integrations/wordpress/tools/i18n.py          update the .po, write the .mo
#   python3 integrations/wordpress/tools/i18n.py --check  fail if a string is untranslated
#
# Collects the strings of the 'owfax-vitrine' text domain in the PHP files and in
# assets/blocks.js, keeps the translations already in languages/owfax-vitrine-fr_FR.po,
# adds the new strings untranslated, drops the vanished ones, and compiles the .mo.

import pathlib
import re
import struct
import sys

PLUGIN = pathlib.Path(__file__).resolve().parent.parent / "owfax-vitrine"
PO = PLUGIN / "languages" / "owfax-vitrine-fr_FR.po"
MO = PO.with_suffix(".mo")
CALL = re.compile(
    r"\b(?:__|_e|esc_html__|esc_attr__|esc_html_e|esc_attr_e)\(\s*"
    r"'((?:[^'\\]|\\.)*)'\s*,\s*'owfax-vitrine'\s*\)"
)
HEADER = (
    "Project-Id-Version: owfax-vitrine\n"
    "Language: fr_FR\n"
    "MIME-Version: 1.0\n"
    "Content-Type: text/plain; charset=UTF-8\n"
    "Content-Transfer-Encoding: 8bit\n"
    "Plural-Forms: nplurals=2; plural=(n > 1);\n"
)


def source_strings():
    out = []
    files = sorted(PLUGIN.rglob("*.php")) + [PLUGIN / "assets" / "blocks.js"]
    for f in files:
        for m in CALL.finditer(f.read_text(encoding="utf-8")):
            s = re.sub(r"\\(.)", r"\1", m.group(1))
            if s not in out:
                out.append(s)
    return out


def po_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def po_unescape(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)


def read_po():
    if not PO.exists():
        return {}
    entries, key, field = {}, None, None
    msgid, msgstr = "", ""
    for line in PO.read_text(encoding="utf-8").splitlines() + [""]:
        if line.startswith("msgid "):
            if key is not None:
                entries[key] = msgstr
            msgid, msgstr, field = po_unescape(line[7:-1]), "", "id"
            key = None
        elif line.startswith("msgstr "):
            msgstr, field, key = po_unescape(line[8:-1]), "str", msgid
        elif line.startswith('"') and field == "id":
            msgid += po_unescape(line[1:-1])
        elif line.startswith('"') and field == "str":
            msgstr += po_unescape(line[1:-1])
    if key is not None:
        entries[key] = msgstr
    entries.pop("", None)
    return entries


def write_po(strings, old):
    lines = ["# French translation of the openWeatherFaxSSB showcase plugin.",
             "# SPDX-License-Identifier: AGPL-3.0-or-later", 'msgid ""', 'msgstr ""']
    lines += ['"' + po_escape(h) + '\\n"' for h in HEADER.splitlines()]
    for s in strings:
        lines += ["", 'msgid "' + po_escape(s) + '"', 'msgstr "' + po_escape(old.get(s, "")) + '"']
    PO.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def mo_bytes(table):
    table = dict(table)
    table[""] = HEADER
    keys = sorted(table)
    ids = [k.encode("utf-8") for k in keys]
    strs = [table[k].encode("utf-8") for k in keys]
    n = len(keys)
    o_ids, o_strs = 28, 28 + 8 * n
    data = 28 + 16 * n
    id_tab, str_tab, blob = [], [], b""
    for b in ids:
        id_tab.append((len(b), data + len(blob)))
        blob += b + b"\0"
    for b in strs:
        str_tab.append((len(b), data + len(blob)))
        blob += b + b"\0"
    out = struct.pack("<7I", 0x950412DE, 0, n, o_ids, o_strs, 0, 0)
    out += b"".join(struct.pack("<2I", *t) for t in id_tab)
    out += b"".join(struct.pack("<2I", *t) for t in str_tab)
    return out + blob


def main():
    strings = source_strings()
    old = read_po()
    missing = [s for s in strings if not old.get(s)]
    if "--check" in sys.argv:
        for s in missing:
            print("untranslated:", s)
        want = mo_bytes({s: old[s] for s in strings if old.get(s)})
        stale = not MO.exists() or MO.read_bytes() != want
        if stale:
            print("the .mo does not match the .po: run tools/i18n.py")
        sys.exit(1 if missing or stale else 0)
    write_po(strings, old)
    MO.write_bytes(mo_bytes({s: old[s] for s in strings if old.get(s)}))
    print(f"{len(strings)} strings, {len(missing)} untranslated")


if __name__ == "__main__":
    main()
