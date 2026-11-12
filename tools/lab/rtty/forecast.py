#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""DWD point forecasts (FEMM74, FEAE75, FEEN73, FEBQ72, FQMM79...) -> tables.

Lab tool, not part of the product (see README.md). Standard library only.

Input: text decoded by rtty.py (with or without --timestamps). Each area looks like
    GOLFE-LION (42.7N   4.5E) SST: 18 C
    FR 13. 00Z: W      5-6   7-8   2  M //
    TH 12. 18Z: S        5   6-7   2  M RAIN   //
i.e. day, time, wind direction, force (Bft), optional gusts (Bft), wave height (m),
optional weather. Lines damaged by RTTY errors are kept apart (never guessed).
"""
import argparse
import datetime as dt
import json
import re
import sys

STAMP = re.compile(r"^\[[^\]]*\]\s?")
HEADING = re.compile(r"\b(F[EQ][A-Z]{2}\d{2})\s+EDZW\s+(\d{6})")
LOOSE_HEADING = re.compile(r"(\S+)\s+EDZ\S*\s+(\S{3,7})")
ISSUE = re.compile(r"FORECAST OF \w\w/(\d{1,2})/(\d{1,2})\.(\d{4})\s+(\d{2})\s*UTC")
# Names may hold digits: FEAE75 has open-sea areas named "49N 10W".
AREA = re.compile(r"^\s*(?P<name>[A-Z0-9][A-Z0-9 .'/-]*?)\s*\(\s*(?P<lat>\d{1,2}\.\d)(?P<ns>[NS])\s+"
                  r"(?P<lon>\d{1,3}\.\d)(?P<ew>[EW])\s*\)\s*(?:SST:\s*(?P<sst>-?\d{1,2})\s*C)?")
DAY = r"(?P<dow>MO|TU|WE|TH|FR|SA|SU)\s*(?P<day>\d{1,2})\.\s*(?P<hour>\d{2})Z:"
ROW = re.compile(
    "^\\s*" + DAY + r"\s*(?P<dir>(?:[NESW]{1,2})(?:-[NESW]{1,2})?|VRB|CALM)?\s+"
    r"(?P<force>\d{1,2}(?:-\d{1,2})?)\s+(?:(?P<gust>\d{1,2}(?:-\d{1,2})?)\s+)?"
    r"(?P<wave>\d{1,2}(?:\.\d)?)\s*M\b\s*(?P<wx>[A-Z +-]*?)\s*(?://)?\s*$")
DAY_START = re.compile("^\\s*" + DAY)
# Damaged area header: still recognisable by "(...N ...)" or "SST". Must close the
# previous area, otherwise its name would be given to another area's rows.
AREA_LOOSE = re.compile(r"\(\s*\d{1,2}[.,]\d?[NS,]|SST|\(.*[EW]\s*\)")
DIRS = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}


def bft_max(s):
    return int(s.split("-")[-1]) if s else None


def clean_lines(text):
    lines = [STAMP.sub("", ln).rstrip() for ln in text.splitlines()]
    # Re-join rows cut by a pause in the transmission (line without its "M ... //").
    out = []
    for ln in lines:
        if out and DAY_START.match(out[-1]) and not out[-1].endswith("//") \
                and not DAY_START.match(ln) and not AREA.match(ln) and ln.strip():
            out[-1] += " " + ln.strip()
        else:
            out.append(ln)
    return out


def parse(text):
    bulletins, cur, area = [], None, None
    issue = None
    def start():
        b = {"heading": None, "time": None, "issue": None, "areas": [], "rejected": []}
        bulletins.append(b)
        return b

    for ln in clean_lines(text):
        # A bulletin starts at ZCZC or at its heading. The heading may be damaged
        # ("AE75 EDZ 8600"): it is then kept as "? (...)" and flagged.
        if ln.lstrip().startswith("ZCZC"):
            cur, area, issue = start(), None, None
        exact, loose = HEADING.search(ln), LOOSE_HEADING.search(ln)
        if exact and cur is not None and cur["heading"] and not cur["heading"].startswith("?"):
            cur, area, issue = start(), None, None  # previous NNNN lost
        if (exact or loose) and (cur is None or (cur["heading"] is None and not cur["areas"])):
            if cur is None:
                cur, area, issue = start(), None, None
            m = exact or loose
            cur["heading"] = m.group(1) if exact else f"? ({m.group(1)})"
            cur["heading_damaged"] = not exact
            cur["time"] = m.group(2)
            continue
        if cur is None or ln.lstrip().startswith("ZCZC"):
            continue
        if ln.strip() in ("NNNN", "="):
            if ln.strip() == "NNNN" or cur["areas"]:
                cur["complete"] = True
            cur, area = None, None
            continue
        m = ISSUE.search(ln)
        if m:
            d, mo, y, h = map(int, m.groups())
            issue = dt.datetime(y, mo, d, h)
            cur["issue"] = issue.strftime("%Y-%m-%dT%HZ")
            continue
        m = AREA.match(ln)
        if m:
            lat = float(m["lat"]) * (1 if m["ns"] == "N" else -1)
            lon = float(m["lon"]) * (1 if m["ew"] == "E" else -1)
            area = {"name": m["name"].strip(), "lat": lat, "lon": lon,
                    "sst_c": int(m["sst"]) if m["sst"] else None, "rows": []}
            cur["areas"].append(area)
            continue
        if AREA_LOOSE.search(ln) and not DAY_START.match(ln):
            area = {"name": f"? ({ln.strip()})", "lat": None, "lon": None, "sst_c": None,
                    "rows": [], "unreadable_header": True}
            cur["areas"].append(area)
            continue
        if area is None:
            continue
        if not DAY_START.match(ln):
            if "//" in ln or re.search(r"\d\s*M", ln):  # a row whose start is damaged
                cur["rejected"].append(f"{area['name']}: {ln.strip()}")
            continue
        m = ROW.match(ln)
        if not m or (m["dir"] and not all(p in DIRS for p in m["dir"].split("-"))
                     and m["dir"] not in ("VRB", "CALM")):
            cur["rejected"].append(f"{area['name']}: {ln.strip()}")
            continue
        row = {"dow": m["dow"], "day": int(m["day"]), "hour": int(m["hour"]),
               "dir": m["dir"], "force": m["force"], "gust": m["gust"],
               "wave_m": float(m["wave"]), "weather": m["wx"].strip() or None, "flags": []}
        if issue:
            row["valid"] = valid_time(issue, row["day"], row["hour"])
        prev = area["rows"][-1] if area["rows"] else None
        if prev and (row.get("valid") or "") < (prev.get("valid") or "") and row.get("valid"):
            # Time went backwards: a new area whose header was lost entirely.
            area = {"name": "? (en-tête perdu)", "lat": None, "lon": None, "sst_c": None,
                    "rows": [], "unreadable_header": True}
            cur["areas"].append(area)
        if bft_max(row["force"]) > 12 or (row["gust"] and bft_max(row["gust"]) > 12) \
                or row["wave_m"] > 20 or row["hour"] > 23 or not 1 <= row["day"] <= 31:
            row["flags"].append("valeur non plausible")
        if row["gust"] and bft_max(row["gust"]) < bft_max(row["force"]):
            row["flags"].append("rafales < vent moyen")
        area["rows"].append(row)
    return bulletins


def valid_time(issue, day, hour):
    """Day of month -> full date, after the issue date (may roll over the month)."""
    y, mo = issue.year, issue.month
    if day < issue.day - 1:
        mo += 1
        if mo == 13:
            y, mo = y + 1, 1
    try:
        return dt.datetime(y, mo, day, hour).strftime("%Y-%m-%dT%HZ")
    except ValueError:
        return None


def render(bulletins, gale=8):
    out = []
    for b in bulletins:
        if not b["areas"]:
            continue  # text-only bulletin or idle traffic
        n = sum(len(a["rows"]) for a in b["areas"])
        out.append(f"=== {b['heading']} (émis {b['issue'] or '?'}) : {len(b['areas'])} zones, "
                   f"{n} échéances, {len(b['rejected'])} lignes illisibles")
        for a in b["areas"]:
            sst = f", mer {a['sst_c']} °C" if a["sst_c"] is not None else ""
            pos = f" ({a['lat']:.1f}, {a['lon']:.1f})" if a["lat"] is not None else "  [ZONE ILLISIBLE]"
            out.append(f"{a['name']}{pos}{sst}")
            for r in a["rows"]:
                wind = f"{r['dir'] or '?':5} F{r['force']:<4}" + (f" raf. F{r['gust']:<4}" if r["gust"] else " " * 11)
                warn = "  << COUP DE VENT" if max(bft_max(r["force"]), bft_max(r["gust"]) or 0) >= gale else ""
                flags = f"  [DOUTEUX : {'; '.join(r['flags'])}]" if r["flags"] else ""
                out.append(f"  {r['dow']} {r['day']:02d} {r['hour']:02d}Z  {wind} vagues {r['wave_m']:>4} m"
                           f"  {r['weather'] or '':5}{warn}{flags}")
        for ln in b["rejected"]:
            out.append(f"  illisible : {ln}")
    return "\n".join(out)


def quality(b):
    """Reception criteria: complete bulletin, readable areas, rejected-row rate."""
    rows = sum(len(a["rows"]) for a in b["areas"])
    rej = len(b["rejected"])
    return {"heading": b["heading"], "heading_ok": not b.get("heading_damaged"),
            "issue_ok": b["issue"] is not None, "complete": bool(b.get("complete")),
            "areas": len(b["areas"]),
            "areas_ok": sum(1 for a in b["areas"] if a["lat"] is not None),
            "rows": rows, "rejected": rej,
            "rejected_pct": round(100 * rej / max(1, rows + rej), 1),
            "flagged": sum(1 for a in b["areas"] for r in a["rows"] if r["flags"])}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("text", nargs="?", help="decoded text file (default stdin)")
    p.add_argument("--json", action="store_true")
    p.add_argument("--quality", action="store_true", help="reception criteria per bulletin")
    p.add_argument("--gale", type=int, default=8, help="Beaufort threshold for the flag")
    a = p.parse_args(argv)
    text = open(a.text, encoding="utf-8").read() if a.text else sys.stdin.read()
    b = parse(text)
    if a.quality:
        for q in (quality(x) for x in b if x["areas"]):
            print(json.dumps(q, ensure_ascii=False))
        return
    if a.json:
        json.dump(b, sys.stdout, ensure_ascii=False, indent=1)
        print()
    else:
        print(render(b, a.gale))


if __name__ == "__main__":
    main()
