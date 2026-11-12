#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""Self-tests of the RTTY / SYNOP proof of concept (no radio needed).

Synthesises an FSK signal (50 Bd, 450 Hz shift, 48 kHz) carrying a SYNOP
bulletin, adds noise and a frequency offset, decodes it and parses it.
Run: python test_rtty.py
"""
import pathlib
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import rtty  # noqa: E402
import forecast  # noqa: E402
import synop  # noqa: E402

BULLETIN = """ZCZC 123
SMDL01 EDZW 111200
AAXX 11124
10147 41558 72315 10084 20061 30112 40123 57012 69911 78162 83570
333 10102 20041 91125=
10400 NIL=
06260 12570 30906 11032 21045 39987 49998 58022 60001 74550=
BBXX
DBAI 11124 99543 10123 41/98 82418 10112 20098 40105 52010 22200 00125 20304 31810 40707=
NNNN
"""


def synth(text, fs=48000, baud=50, center=1910.0, shift=450, mark_high=True, snr_db=10,
          lead_s=1.0, seed=1):
    """Continuous-phase FSK, 1 start + 5 data + 1.5 stop; SNR measured in 3 kHz."""
    rng = np.random.default_rng(seed)
    spb = fs / baud
    bits = []  # (level, duration in bits)
    for c in [rtty.LTRS] * 3 + rtty.ita2_encode(text):
        bits += [(0, 1)] + [((c >> k) & 1, 1) for k in range(5)] + [(1, 1.5)]
    sym = [np.ones(int(lead_s * fs))]
    acc = 0.0
    for level, n in bits:
        a, acc = int(round(acc)), acc + n * spb
        sym.append(np.full(int(round(acc)) - a, float(level)))
    sym.append(np.ones(fs))
    sym = np.concatenate(sym)
    hi_tone = sym if mark_high else 1 - sym
    f = center - shift / 2 + shift * hi_tone
    x = np.sin(2 * np.pi * np.cumsum(f) / fs)
    # Signal power 0.5; noise power in 3 kHz = 0.5 / snr -> total = that * (fs/2) / 3000.
    sigma = np.sqrt(0.5 / 10 ** (snr_db / 10) * (fs / 2) / 3000)
    x += rng.normal(0, sigma, len(x))
    return (x / np.max(np.abs(x)) * 0.5 * 32767).astype("<i2")


def write_wav(path, x, fs=48000):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(x.tobytes())


def norm(s):
    return " ".join(s.split())


def test_ita2_roundtrip():
    s = "AAXX 11124 10147 41558=\nSEA WEATHER REPORT, 1200 UTC: SW 5-6."
    assert rtty.ita2_decode(rtty.ita2_encode(s)) == s.upper()


def test_decode(mark_high, snr_db, max_cer):
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "t.wav"
        write_wav(p, synth(BULLETIN, mark_high=mark_high, snr_db=snr_db))
        r = rtty.decode(p)
    got = norm(rtty.render(r))
    want = norm(BULLETIN)
    errors = sum(a != b for a, b in zip(got, want)) + abs(len(got) - len(want))
    cer = errors / len(want)
    print(f"  mark {'high' if mark_high else 'low'}, SNR {snr_db} dB in 3 kHz: center "
          f"{r['center_hz']:.0f} Hz, detected mark {r['mark']}, {r['framing_errors']} framing "
          f"errors, CER {100 * cer:.1f} %")
    assert abs(r["center_hz"] - 1910) < 5
    assert r["mark"] == ("high" if mark_high else "low")
    assert cer <= max_cer, got
    return got


def test_synop():
    reps = synop.parse_text(BULLETIN)
    assert len(reps) == 4, reps
    a, nil, b, ship = reps
    assert a["station"] == "10147" and a["day"] == 11 and a["hour"] == 12
    assert a["wind_dir"] == 230 and a["wind_kt"] == 15 and a["beaufort"] == 4
    assert a["temp_c"] == 8.4 and a["dewpoint_c"] == 6.1
    assert a["station_hpa"] == 1011.2 and a["msl_hpa"] == 1012.3
    assert a["tendency_hpa"] == -1.2 and a["visibility_m"] == 8000
    assert a["precip_mm"] == 0.1 and a["precip_h"] == 6
    assert a["weather"].startswith("averses de pluie modérées")
    assert a["tmax_c"] == 10.2 and a["tmin_c"] == 4.1 and a["gust_kt"] == 25
    assert not a["flags"], a["flags"]
    assert nil.get("nil")
    assert b["temp_c"] == -3.2 and b["msl_hpa"] == 999.8 and b["visibility_m"] == 20000
    assert b["wind_dir"] == 90 and b["wind_kt"] == 6
    assert ship["callsign"] == "DBAI" and ship["lat"] == 54.3 and ship["lon"] == 12.3
    assert ship["sea_temp_c"] == 12.5 and ship["waves_m"] == 2.0 and ship["swell_dir"] == 180
    for r in reps:
        print("  " + synop.describe(r))
    # Corrupted group (RTTY error) is flagged, not silently trusted.
    bad = synop.parse_text("AAXX 11124 10147 41558 72315 1Q084 20061 49999=")[0]
    assert any("illisible" in f for f in bad["flags"])
    # Real report (DDH7 2026-11-11 18Z): radiation group 20000 after 553// is not Tmin;
    # 910 and 911 gusts: the highest is kept.
    real = synop.parse_text("AAXX 11184 06193 07482 72211 10062 20033 30017 40029 58010 "
                            "60005 700// 87/// 333 10080 20062 55000 553// 20000 60007 87/16 "
                            "91018 90710 91118=")[0]
    assert real["tmin_c"] == 6.2 and real["tmax_c"] == 8.0 and real["gust_kt"] == 18, real
    # Real damaged ship report (digit lost in 99LaLaLa): values withheld, not shifted.
    # The bulletin had lost its NNNN: the following CQ call must not be absorbed.
    reps = synop.parse_text("BBXX 3FLO4 11184 9933 10140 41798 52120 10200 20167 40155=\n"
                            "WGAE 1118/ 99314 10323 41/// ///// 10195\nCQ CQ CQ DE DDK2 DDH7")
    assert reps[0].get("broken") and "ILLISIBLE" in synop.describe(reps[0])
    assert len(reps) == 2 and not any("CQ" in f for f in reps[1]["flags"]), reps[1]
    hot = synop.parse_text("AAXX 11124 10147 41558 72315 10984 20061=")[0]
    assert any("non plausible" in f for f in hot["flags"])


FORECAST = """ZCZC 159
FEMM74 EDZW 110600
DWD FORECAST OF WE/11/11.2026 00 UTC:
WIND FORCE: BEAUFORT, WAVE HEIGHT: METRE
GOLFE-LION (42.7N   4.5E) SST: 18 C
TH 12. 00Z: S       4         1  M //
FR 13. 00Z: W      5-6   7-8   2  M RAIN   //
=:94'/SARD.-W (41.3N   85E) SST: 21 C
TH 12. 00Z: S-SW   4-5         1  M //
TH 12. 12Z: SW    #  6   7-8  2.5 M //
=
NNNN
"""


def test_forecast():
    (b,) = forecast.parse(FORECAST)
    assert b["heading"] == "FEMM74" and b["issue"] == "2026-11-11T00Z"
    gl, bad = b["areas"]
    assert gl["name"] == "GOLFE-LION" and gl["lat"] == 42.7 and gl["sst_c"] == 18
    r = gl["rows"][1]
    assert (r["dir"], r["force"], r["gust"], r["wave_m"], r["weather"], r["valid"]) ==         ("W", "5-6", "7-8", 2.0, "RAIN", "2026-11-13T00Z")
    # Damaged header: rows must not be attributed to GOLFE-LION.
    assert bad.get("unreadable_header") and len(bad["rows"]) == 1
    assert len(b["rejected"]) == 1
    # FEAE75 open-sea area named by its position (DDH7 2026-11-12 18Z).
    (b,) = forecast.parse("FEAE75 EDZW 121200\n49N 10W (49.0N  10.0W) SST: 15 C\n"
                          "TH 12. 18Z: SW     6-7   8   3.5  M //\nNNNN\n")
    (a,) = b["areas"]
    assert a["name"] == "49N 10W" and a["lon"] == -10.0 and len(a["rows"]) == 1, a


if __name__ == "__main__":
    test_ita2_roundtrip()
    print("ita2 ok")
    test_synop()
    print("synop ok")
    test_forecast()
    print("forecast ok")
    test_decode(True, 20, 0.0)
    test_decode(False, 10, 0.01)
    test_decode(True, 3, 0.05)
    print("decode ok")
