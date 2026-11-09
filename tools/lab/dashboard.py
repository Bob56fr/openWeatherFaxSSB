#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""Read-only lab dashboard for the development station.

Shows, refreshed every 15 s: running captures, the operations journal, the S-meter
curve, a probe of the last 30 s of the recording in progress, and decoded charts.
Never touches the radio: it only reads files written by the capture scripts.

Usage: dashboard.py [--port 8090] [--corpus ~/owfax-corpus] [--cli path/to/owfax-cli]
Journal: append lines to <corpus>/journal.log (e.g. `owfax-journal "message"`).
"""

import argparse
import datetime as dt
import glob
import html
import http.server
import os
import subprocess
import tempfile
import urllib.parse
import wave

ARGS = None


def tail(path, n=40):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.readlines()[-n:]
    except OSError:
        return []


def processes():
    out = subprocess.run(["ps", "-eo", "pid,etime,args"], capture_output=True, text=True).stdout
    keep = ("ic7300_capture", "ic7300_scan", "arecord", "rigctld", "owfax-cli")
    return [l for l in out.splitlines()[1:] if any(k in l for k in keep) and "ps -eo" not in l]


def smeter_svg(csv_path, width=900, height=160):
    rows = []
    for line in tail(csv_path, 2000)[1:] if os.path.exists(csv_path) else []:
        try:
            t, v = line.strip().split(",")
            rows.append((t, float(v)))
        except ValueError:
            continue
    if len(rows) < 2:
        return "<p class=muted>Pas encore de mesures.</p>"
    lo, hi = -60.0, 20.0
    pts = []
    for i, (_, v) in enumerate(rows):
        x = 40 + i * (width - 50) / (len(rows) - 1)
        y = 10 + (hi - max(lo, min(hi, v))) * (height - 30) / (hi - lo)
        pts.append(f"{x:.1f},{y:.1f}")
    grid = ""
    for db in (-60, -40, -20, 0, 20):
        y = 10 + (hi - db) * (height - 30) / (hi - lo)
        grid += (f'<line x1="40" x2="{width-10}" y1="{y:.1f}" y2="{y:.1f}" class="grid"/>'
                 f'<text x="4" y="{y+4:.1f}" class="axis">{db}</text>')
    return (f'<svg viewBox="0 0 {width} {height}" class="chart">{grid}'
            f'<polyline points="{" ".join(pts)}" class="line"/>'
            f'<text x="40" y="{height-4}" class="axis">{rows[0][0]} UTC</text>'
            f'<text x="{width-90}" y="{height-4}" class="axis">{rows[-1][0]} UTC</text></svg>')


def probe_tail(wav_path, seconds=30):
    """Copies the last `seconds` of a WAV being written and runs owfax-cli probe."""
    try:
        size = os.path.getsize(wav_path)
        with open(wav_path, "rb") as f:
            header = f.read(44)
            rate = int.from_bytes(header[24:28], "little")
            frame = 2
            n = min((size - 44) // frame, rate * seconds)
            f.seek(44 + ((size - 44) // frame - n) * frame)
            data = f.read(n * frame)
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            w = wave.open(tmp.name, "wb")
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(data)
            w.close()
            out = subprocess.run([ARGS.cli, "probe", tmp.name], capture_output=True,
                                 text=True, timeout=30).stdout.strip()
        os.unlink(tmp.name)
        return out
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        return f"(probe indisponible : {e})"


def page():
    corpus = ARGS.corpus
    now = dt.datetime.now(dt.timezone.utc)
    paris = now.astimezone(dt.timezone(dt.timedelta(hours=1)))  # CET (winter)
    wavs = sorted(glob.glob(os.path.join(corpus, "*.wav")), key=os.path.getmtime)
    latest = wavs[-1] if wavs else None
    procs = processes()
    recording = any("arecord" in p for p in procs)

    parts = [f"<h1>openWeatherFaxSSB — labo <span class=muted>{html.escape(os.uname().nodename)}</span></h1>",
             f"<p class=muted>{now:%Y-%m-%d %H:%M:%S} UTC · {paris:%H:%M} Paris (hiver) · "
             "rafraîchi toutes les 15 s · lecture seule</p>"]

    status = "● Enregistrement en cours" if recording else "○ Aucun enregistrement"
    parts.append(f'<h2 class="{"live" if recording else ""}">{status}</h2>')
    parts.append("<pre>" + html.escape("\n".join(procs) or "—") + "</pre>")

    parts.append("<h2>Journal des opérations</h2><pre>" +
                 html.escape("".join(tail(os.path.join(corpus, "journal.log"), 25)) or "—") + "</pre>")

    if latest:
        base = latest[:-4]
        size_mb = os.path.getsize(latest) / 1e6
        parts.append(f"<h2>Capture : {html.escape(os.path.basename(latest))}</h2>")
        parts.append(f"<p>{size_mb:.1f} Mo · {html.escape(''.join(tail(base + '.json', 30)))[:600]}</p>")
        parts.append("<h3>S-mètre (dB)</h3>" + smeter_svg(base + ".smeter.csv"))
        if recording:
            parts.append("<h3>Analyse des 30 dernières secondes</h3><pre>" +
                         html.escape(probe_tail(latest)) + "</pre>")
        for log in sorted(glob.glob(os.path.join(corpus, "capture*.log"))):
            parts.append(f"<h3>{html.escape(os.path.basename(log))}</h3><pre>" +
                         html.escape("".join(tail(log, 12))) + "</pre>")

    pngs = sorted(glob.glob(os.path.join(corpus, "decoded", "*.png")), key=os.path.getmtime,
                  reverse=True)[:12]
    parts.append("<h2>Cartes décodées</h2>")
    if not pngs:
        parts.append("<p class=muted>Aucune pour l'instant.</p>")
    for p in pngs:
        name = os.path.basename(p)
        url = "/img/" + urllib.parse.quote(name)
        parts.append(f'<figure><a href="{url}"><img src="{url}" loading="lazy" alt="{html.escape(name)}"></a>'
                     f"<figcaption>{html.escape(name)}</figcaption></figure>")

    return f"""<!doctype html><html lang=fr><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta http-equiv=refresh content=15><title>owfax labo</title><style>
:root {{ --bg:#fbfaf7; --fg:#1d1d1b; --muted:#6b6a65; --line:#2563eb; --grid:#e3e1da; --live:#b91c1c; --code:#f1efe9; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#161614; --fg:#ecebe6; --muted:#9a988f; --line:#60a5fa; --grid:#33322e; --live:#f87171; --code:#22211e; }} }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.45 system-ui,sans-serif; margin:0 auto; max-width:1000px; padding:16px; }}
h1 {{ font-size:1.3rem; }} h2 {{ font-size:1.05rem; margin-top:1.6em; }} h3 {{ font-size:.95rem; }}
.muted {{ color:var(--muted); font-weight:normal; }} .live {{ color:var(--live); }}
pre {{ background:var(--code); padding:8px; overflow-x:auto; font-size:12px; white-space:pre-wrap; }}
.chart {{ width:100%; height:auto; }} .grid {{ stroke:var(--grid); }} .axis {{ fill:var(--muted); font-size:11px; }}
.line {{ fill:none; stroke:var(--line); stroke-width:1.5; }}
figure {{ margin:0 0 16px; }} img {{ max-width:100%; border:1px solid var(--grid); background:#fff; }}
figcaption {{ color:var(--muted); font-size:12px; }}
</style></head><body>{"".join(parts)}</body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/img/"):
            name = os.path.basename(urllib.parse.unquote(self.path[5:]))
            path = os.path.join(ARGS.corpus, "decoded", name)
            if name.endswith(".png") and os.path.isfile(path):
                with open(path, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(404)
            return
        body = page().encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    global ARGS
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--corpus", default=os.path.expanduser("~/owfax-corpus"))
    ap.add_argument("--cli", default=os.path.join(here, "..", "..", "build", "apps", "cli", "owfax-cli"))
    ARGS = ap.parse_args()
    os.makedirs(os.path.join(ARGS.corpus, "decoded"), exist_ok=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
