#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""SYNOP (FM 12) / SHIP (FM 13) proof of concept: decoded RTTY text -> plain French.

Lab tool, not part of the product (see README.md). Standard library only.
Covers sections 0, 1, 2 (ship: sea temperature, waves, swell) and part of 3
(max/min temperature, snow depth, gusts). Section 5 (national) is ignored.
Groups that are not 5 characters of digits or '/' (RTTY errors) are skipped and
the report is flagged; implausible values are flagged too, never shown as sure.
"""
import argparse
import json
import re
import sys

GROUP = re.compile(r"^[0-9/]{5}$")
HEADING = re.compile(r"^[A-Z]{4}\d{2}$")  # WMO abbreviated heading, e.g. SMDL01

TENDENCY = ["hausse puis baisse", "hausse puis stable", "hausse", "baisse ou stable puis hausse",
            "stable", "baisse puis hausse", "baisse puis stable", "baisse",
            "stable ou hausse puis baisse"]
CLOUD_BASE = ["<50 m", "50-100 m", "100-200 m", "200-300 m", "300-600 m", "600-1000 m",
              "1000-1500 m", "1500-2000 m", "2000-2500 m", ">=2500 m ou pas de nuages"]
PRECIP_PERIOD = {1: 6, 2: 12, 3: 18, 4: 24, 5: 1, 6: 2, 7: 3, 8: 9, 9: 15}
QUADRANT = {1: (1, 1), 3: (-1, 1), 5: (-1, -1), 7: (1, -1)}  # Qc -> (lat sign, lon sign)
WW = [  # (first, last, text)
    (0, 3, "pas de temps significatif"), (4, 4, "fumée"), (5, 5, "brume sèche"),
    (6, 9, "poussière, sable en suspension"), (10, 10, "brume"),
    (11, 12, "brouillard bas"), (13, 13, "éclairs"), (14, 16, "précipitations en vue"),
    (17, 17, "orage sans précipitations"), (18, 18, "grains"), (19, 19, "trombe"),
    (20, 29, "phénomène dans l'heure passée"), (30, 35, "tempête de sable"),
    (36, 39, "chasse-neige"), (40, 49, "brouillard"), (50, 55, "bruine"),
    (56, 57, "bruine verglaçante"), (58, 59, "bruine et pluie"), (60, 65, "pluie"),
    (66, 67, "pluie verglaçante"), (68, 69, "pluie et neige"), (70, 75, "neige"),
    (76, 79, "cristaux, grains de neige ou grésil"), (80, 80, "averses de pluie faibles"),
    (81, 81, "averses de pluie modérées ou fortes"), (82, 82, "averses de pluie violentes"),
    (83, 84, "averses de pluie et neige"), (85, 86, "averses de neige"),
    (87, 88, "averses de grésil"), (89, 90, "averses de grêle"),
    (91, 94, "précipitations, orage dans l'heure passée"), (95, 95, "orage"),
    (96, 96, "orage avec grêle"), (97, 97, "orage fort"),
    (98, 98, "orage avec tempête de sable"), (99, 99, "orage fort avec grêle"),
]


def num(s):
    return None if "/" in s else int(s)


def ww_text(ww):
    for lo, hi, text in WW:
        if lo <= ww <= hi:
            if 50 <= ww <= 75 and ww not in (56, 57, 58, 59, 66, 67, 68, 69):
                text += " " + ["faible", "modérée", "forte"][((ww % 10) % 6) // 2]
            return text
    return f"code {ww}"


def visibility_m(vv):
    if vv <= 50:
        return vv * 100
    if 56 <= vv <= 80:
        return (vv - 50) * 1000
    if 81 <= vv <= 88:
        return (30 + (vv - 80) * 5) * 1000
    if vv == 89:
        return 70000
    if vv >= 90:
        return [50, 50, 200, 500, 1000, 2000, 4000, 10000, 20000, 50000][vv - 90]
    return None  # 51-55 unused


def temp(g):  # snTTT, sn 0 positive / 1 negative, tenths
    sn, t = g[1], num(g[2:5])
    if t is None or sn not in "01":
        return None
    return (-t if sn == "1" else t) / 10


def pressure(g):  # PPPP in tenths, thousands digit dropped
    p = num(g[1:5])
    if p is None:
        return None
    p /= 10
    return p + 1000 if p < 100 else p


def beaufort(kt):
    return sum(kt >= t for t in (1, 4, 7, 11, 17, 22, 28, 34, 41, 48, 56, 64))


def parse_report(tokens, kind, day, hour, wind_unit):
    r = {"type": kind, "day": day, "hour": hour, "flags": []}
    groups = []
    # Section 1 is read by position up to Nddff (land: IIiii iRixhVV Nddff; ship:
    # 99LaLaLa QcLoLoLoLo iRixhVV Nddff). An unreadable token there shifts every
    # following group and yields plausible but wrong values: the report is withheld.
    head = 4 if kind == "SHIP" else 3
    for pos, t in enumerate(tokens):
        if t == "NIL":
            r["nil"] = True
        elif GROUP.match(t) or t in ("333", "555"):
            groups.append(t)
        else:
            r["flags"].append(f"groupe illisible '{t}'")
            if pos < head:
                r["broken"] = True
    if not groups:
        return r
    it = iter(groups)
    try:
        if kind == "SHIP":
            # Callsign was taken by the caller; then 99LaLaLa QcLoLoLoLo.
            g = next(it)
            if g.startswith("99"):
                la, g2 = num(g[2:5]), next(it)
                qc, lo = num(g2[0]), num(g2[1:5])
                if None not in (la, qc, lo) and qc in QUADRANT:
                    sla, slo = QUADRANT[qc]
                    r["lat"], r["lon"] = sla * la / 10, slo * lo / 10
                g = next(it)
        else:
            g = groups[0]
            r["station"] = g
            next(it)
            g = next(it)
        if r.get("nil"):
            return r
        # iRixhVV
        h, vv = num(g[2]), num(g[3:5])
        if h is not None:
            r["cloud_base"] = CLOUD_BASE[h]
        if vv is not None:
            r["visibility_m"] = visibility_m(vv)
        # Nddff
        g = next(it)
        n, dd, ff = num(g[0]), num(g[1:3]), num(g[3:5])
        if n is not None:
            r["cloud_okta"] = n
        if ff == 99:
            ff = num(next(it)[2:5])
        if dd is not None and ff is not None:
            kt = ff if wind_unit == "kt" else ff * 1.943844
            r["wind_dir"] = None if dd == 99 else dd * 10
            r["wind_kt"] = round(kt)
            r["beaufort"] = beaufort(kt)
            if dd not in range(0, 37) and dd != 99:
                r["flags"].append(f"direction du vent {dd}")
        section, last, after5 = 1, 0, False
        for g in it:
            if g == "333":
                section = 3
                continue
            if g == "555":
                break
            if section == 1 and g.startswith("222"):
                section = 2
                continue
            i = g[0]
            if section == 1:
                if int(i) < last if i.isdigit() else False:
                    r["flags"].append(f"groupe hors d'ordre {g}")
                last = int(i) if i.isdigit() else last
                if i == "1":
                    r["temp_c"] = temp(g)
                elif i == "2" and g[1] == "9":
                    r["humidity_pct"] = num(g[2:5])
                elif i == "2":
                    r["dewpoint_c"] = temp(g)
                elif i == "3":
                    r["station_hpa"] = pressure(g)
                elif i == "4" and g[1] in "09":
                    r["msl_hpa"] = pressure(g)
                elif i == "5":
                    a, ppp = num(g[1]), num(g[2:5])
                    if a is not None and a <= 8 and ppp is not None:
                        sign = 1 if a < 4 else (0 if a == 4 else -1)
                        r["tendency_hpa"] = sign * ppp / 10
                        r["tendency"] = TENDENCY[a]
                elif i == "6":
                    rrr, tr = num(g[1:4]), num(g[4])
                    if rrr is not None:
                        mm = 0.0 if rrr == 990 else (rrr - 990) / 10 if rrr > 990 else float(rrr)
                        r["precip_mm"] = mm
                        r["precip_h"] = PRECIP_PERIOD.get(tr)
                elif i == "7":
                    ww = num(g[1:3])
                    if ww is not None:
                        r["ww"], r["weather"] = ww, ww_text(ww)
            elif section == 2:
                if i == "0":
                    t = num(g[2:5])
                    if t is not None and g[1] in "01234567":
                        r["sea_temp_c"] = (-t if int(g[1]) % 2 else t) / 10
                elif i in "12":  # wave period (s), height (0.5 m units)
                    p, hh = num(g[1:3]), num(g[3:5])
                    if hh is not None:
                        r["waves_m"] = hh / 2
                    if p is not None:
                        r["waves_s"] = p
                elif i == "3":
                    d1 = num(g[1:3])
                    if d1 is not None:
                        r["swell_dir"] = d1 * 10
                elif i == "4":
                    p, hh = num(g[1:3]), num(g[3:5])
                    if hh is not None:
                        r["swell_m"] = hh / 2
                    if p is not None:
                        r["swell_s"] = p
                elif i == "7" and g[1] == "0":
                    hh = num(g[2:5])
                    if hh is not None:
                        r["waves_m"] = hh / 10
            else:  # section 3
                if i == "5":
                    after5 = True  # 55SSS etc. are followed by radiation groups 0-6FFFF
                elif after5 and i in "01234567" and i != "7":
                    continue
                if i == "1" and "tmax_c" not in r:
                    r["tmax_c"] = temp(g)
                elif i == "2" and "tmin_c" not in r:
                    r["tmin_c"] = temp(g)
                elif i == "4":
                    sss = num(g[2:5])
                    if sss is not None and sss < 997:
                        r["snow_cm"] = sss
                elif g.startswith("910") or g.startswith("911"):
                    ff = num(g[3:5])
                    if ff is not None:  # 910 (10 min) and 911 (period): keep the highest
                        kt = round(ff if wind_unit == "kt" else ff * 1.943844)
                        r["gust_kt"] = max(kt, r.get("gust_kt", 0))
    except StopIteration:
        r["flags"].append("message tronqué")
    check(r)
    return r


def check(r):
    for key, lo, hi in (("temp_c", -80, 60), ("dewpoint_c", -90, 40), ("msl_hpa", 870, 1085),
                        ("station_hpa", 500, 1085), ("wind_kt", 0, 150), ("gust_kt", 0, 200),
                        ("sea_temp_c", -3, 40), ("waves_m", 0, 25)):
        v = r.get(key)
        if v is not None and not lo <= v <= hi:
            r["flags"].append(f"{key}={v} non plausible")
    t, td = r.get("temp_c"), r.get("dewpoint_c")
    if t is not None and td is not None and td > t + 0.5:
        r["flags"].append("point de rosée > température")


def parse_text(text):
    """Finds AAXX/BBXX bulletins in free text; returns a list of report dicts."""
    text = re.sub(r"(?m)^\[[^\]]*\]\s?", "", text)  # rtty.py --timestamps prefixes
    tokens = text.upper().replace("=", " = ").split()
    reports, mode, cur, heading = [], None, [], None
    day = hour = unit = None
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("AAXX", "BBXX") and i + 1 < len(tokens):
            if cur and mode:
                reports.append(finish(cur, mode, day, hour, unit, heading, truncated=True))
            cur = []
            mode = "SYNOP" if t == "AAXX" else "SHIP"
            if mode == "SYNOP":
                yyggi = tokens[i + 1]
                day, hour, unit = header(yyggi)
                i += 2
                continue
            i += 1
            continue
        if HEADING.match(t) or t in ("NNNN", "ZCZC", "CQ"):  # also ends a bulletin whose NNNN was lost
            if cur and mode:
                reports.append(finish(cur, mode, day, hour, unit, heading, truncated=True))
            cur, mode = [], None
            if HEADING.match(t):
                heading = " ".join(tokens[i:i + 3])
            i += 1
            continue
        if mode:
            if t == "=":
                if cur:
                    reports.append(finish(cur, mode, day, hour, unit, heading))
                cur = []
            else:
                cur.append(t)
        i += 1
    if cur and mode:
        reports.append(finish(cur, mode, day, hour, unit, heading, truncated=True))
    return reports


def header(yyggi):
    if not GROUP.match(yyggi):
        return None, None, "kt"
    yy, gg, iw = num(yyggi[0:2]), num(yyggi[2:4]), num(yyggi[4])
    return yy, gg, "kt" if iw in (3, 4) else "m/s"


def finish(cur, mode, day, hour, unit, heading, truncated=False):
    if mode == "SHIP":
        call = cur[0]
        day, hour, unit = header(cur[1]) if len(cur) > 1 else (None, None, "kt")
        r = parse_report(cur[2:], "SHIP", day, hour, unit)
        r["callsign"] = call
        if len(cur) < 2 or not GROUP.match(cur[1]):
            r["broken"] = True
            r["flags"].append(f"groupe date illisible '{cur[1] if len(cur) > 1 else ''}'")
    else:
        r = parse_report(cur, "SYNOP", day, hour, unit)
    if heading:
        r["heading"] = heading
    if truncated:
        r["flags"].append("fin de message ('=') absente")
    return r


def fmt(v, unit="", dec=1):
    return f"{v:.{dec}f}".replace(".", ",") + unit


def describe(r, names=None):
    who = r.get("station") or r.get("callsign") or "?"
    if names and who in names:
        who += f" {names[who]}"
    if "lat" in r:
        who += f" ({fmt(r['lat'], '°')} {fmt(r['lon'], '°')})"
    when = f"J{r['day']:02d} {r['hour']:02d}h UTC" if r.get("day") is not None and r.get("hour") is not None else ""
    if r.get("nil"):
        if who == "?" and r.get("heading"):
            who = f"bulletin {r['heading']}"
        return f"{who} {when} : pas d'observation (NIL)"
    parts = []
    if "wind_kt" in r:
        d = "variable" if r["wind_dir"] is None else ("calme" if r["wind_kt"] == 0 else f"{r['wind_dir']:03d}°")
        parts.append(f"vent {d} {r['wind_kt']} kt (F{r['beaufort']})")
    if "gust_kt" in r:
        parts.append(f"rafales {r['gust_kt']} kt")
    if r.get("temp_c") is not None:
        parts.append(f"T {fmt(r['temp_c'], ' °C')}")
    if r.get("dewpoint_c") is not None:
        parts.append(f"Td {fmt(r['dewpoint_c'], ' °C')}")
    if r.get("msl_hpa") is not None:
        p = f"P {fmt(r['msl_hpa'], ' hPa')}"
        if "tendency_hpa" in r:
            p += f" ({'+' if r['tendency_hpa'] > 0 else ''}{fmt(r['tendency_hpa'])}/3 h, {r['tendency']})"
        parts.append(p)
    if r.get("visibility_m") is not None:
        v = r["visibility_m"]
        parts.append(f"visi {v} m" if v < 5000 else f"visi {v // 1000} km")
    if "weather" in r:
        parts.append(r["weather"])
    if "cloud_okta" in r:
        parts.append("ciel invisible" if r["cloud_okta"] == 9 else f"nébulosité {r['cloud_okta']}/8")
    if r.get("precip_mm") is not None:
        parts.append(f"précip {fmt(r['precip_mm'], ' mm')}" + (f"/{r['precip_h']} h" if r.get("precip_h") else ""))
    if r.get("sea_temp_c") is not None:
        parts.append(f"mer {fmt(r['sea_temp_c'], ' °C')}")
    if "waves_m" in r:
        parts.append(f"vagues {fmt(r['waves_m'], ' m')}" + (f" {r['waves_s']} s" if r.get("waves_s") else ""))
    if "swell_m" in r:
        parts.append(f"houle {r.get('swell_dir', '?')}° {fmt(r['swell_m'], ' m')}")
    if r.get("tmax_c") is not None:
        parts.append(f"Tmax {fmt(r['tmax_c'], ' °C')}")
    if r.get("tmin_c") is not None:
        parts.append(f"Tmin {fmt(r['tmin_c'], ' °C')}")
    if "snow_cm" in r:
        parts.append(f"neige {r['snow_cm']} cm")
    if r.get("broken"):
        return f"{who} {when} : ILLISIBLE, valeurs non affichées (début du message abîmé : " \
               + "; ".join(r["flags"]) + ")"
    line = f"{who} {when} : " + ", ".join(parts)
    if r["flags"]:
        line += "  [DOUTEUX : " + "; ".join(r["flags"]) + "]"
    return line


def load_names(path):
    """Optional station list: 'index;name' per line (e.g. extracted from OSCAR/Surface)."""
    names = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split(";")
            if len(parts) >= 2 and parts[0].isdigit():
                names[parts[0].zfill(5)] = parts[1]
    return names


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("text", nargs="?", help="decoded text file (default stdin)")
    p.add_argument("--json", action="store_true")
    p.add_argument("--stations", help="'index;name' file")
    a = p.parse_args(argv)
    text = open(a.text, encoding="utf-8").read() if a.text else sys.stdin.read()
    reports = parse_text(text)
    if a.json:
        json.dump(reports, sys.stdout, ensure_ascii=False, indent=1)
        print()
        return
    names = load_names(a.stations) if a.stations else None
    for r in reports:
        print(describe(r, names))
    bad = sum(1 for r in reports if r["flags"])
    print(f"{len(reports)} observations, {bad} douteuses", file=sys.stderr)


if __name__ == "__main__":
    main()
