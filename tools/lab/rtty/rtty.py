#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 openWeatherFaxSSB contributors
"""RTTY proof of concept: 16-bit WAV recording -> ITA2 text.

Lab tool, not part of the product (see README.md). Requires numpy.

Chain: FIR low-pass + decimation to ~8 kHz, tone pair search on the averaged
spectrum (then fine shift search), two quadrature detectors integrated over
0.75 bit, automatic threshold correction (each tone normalised by its own
running peak, against selective fading), asynchronous
slicer (1 start, 5 data LSB first, 1.5 stop), ITA2 LTRS/FIGS decoding.
Polarity (mark high or low) is chosen by trying both.
"""
import argparse
import datetime as dt
import json
import pathlib
import sys
import wave

import numpy as np

LTRS, FIGS = 31, 27
LETTERS = list("\0E\nA SIU\rDRJNFCKTZLWHYPQOBG") + [None, "M", "X", "V", None]
FIGURES = ["\0", "3", "\n", "-", " ", "'", "8", "7", "\r", None, "4", None, ",", "!",
           ":", "(", "5", "+", ")", "2", "#", "6", "0", "1", "9", "?", "&", None,
           ".", "/", "=", None]

# ---------------------------------------------------------------- ITA2


def ita2_encode(text):
    """Text -> list of 5-bit codes (starts in LTRS). Unknown characters dropped."""
    codes, figs = [LTRS], False
    for ch in text.upper():
        if ch in " \n\r":
            codes.append(LETTERS.index(ch))
        elif ch in LETTERS:
            if figs:
                codes.append(LTRS)
                figs = False
            codes.append(LETTERS.index(ch))
        elif ch in FIGURES:
            if not figs:
                codes.append(FIGS)
                figs = True
            codes.append(FIGURES.index(ch))
    return codes


def ita2_decode(codes, usos=False):
    """Codes -> text. usos: unshift on space (back to LTRS after a space)."""
    out, figs = [], False
    for c in codes:
        if c == LTRS:
            figs = False
        elif c == FIGS:
            figs = True
        else:
            ch = (FIGURES if figs else LETTERS)[c]
            if ch == " " and usos:
                figs = False
            if ch and ch not in "\0\r":
                out.append(ch)
    return "".join(out)

# ---------------------------------------------------------------- DSP


def lowpass(cutoff, fs, n=129):
    t = np.arange(n) - (n - 1) / 2
    h = np.sinc(2 * cutoff / fs * t) * np.hamming(n)
    return (h / h.sum()).astype(np.float32)


def read_decimated(path, target=8000, chunk_s=10):
    """Reads a WAV in chunks, low-pass at 3 kHz and decimates by an integer factor."""
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit("16-bit PCM WAV expected")
        fs, ch = w.getframerate(), w.getnchannels()
        q = max(1, fs // target)
        h = lowpass(min(3000, 0.45 * fs / q), fs)
        hist = np.zeros(len(h) - 1, np.float32)
        pos, parts = 0, []
        while True:
            raw = w.readframes(fs * chunk_s)
            if not raw:
                break
            x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768
            if ch > 1:
                x = x.reshape(-1, ch)[:, 0]
            buf = np.concatenate([hist, x])
            y = np.convolve(buf, h, "valid")
            parts.append(y[(-pos) % q::q])
            pos += len(x)
            hist = buf[-(len(h) - 1):]
    return np.concatenate(parts) if parts else np.zeros(0, np.float32), fs / q


def find_center(x, fs, shift, lo=300.0, hi=None):
    """Center frequency maximising the weaker of the two tones (both must be present)."""
    nfft = 8192
    if len(x) < nfft:
        raise SystemExit("recording too short")
    win = np.hanning(nfft).astype(np.float32)
    acc = np.zeros(nfft // 2 + 1)
    hops = range(0, len(x) - nfft, nfft // 2)
    for i in hops:
        acc += np.abs(np.fft.rfft(x[i:i + nfft] * win)) ** 2
    acc /= len(hops)
    df = fs / nfft
    k = max(1, int(round(8 / df)))  # +-8 Hz around each tone
    smooth = np.convolve(acc, np.ones(2 * k + 1), "same")
    hi = hi or fs / 2 - shift / 2 - 200
    centers = np.arange(lo + shift / 2, hi, 1.0)
    m = np.interp(centers + shift / 2, np.arange(len(smooth)) * df, smooth)
    s = np.interp(centers - shift / 2, np.arange(len(smooth)) * df, smooth)
    score = np.minimum(m, s)
    best = int(np.argmax(score))
    noise = np.median(smooth[int(lo / df):int(hi / df)])
    snr_db = 10 * np.log10(score[best] / noise) if noise > 0 else float("nan")
    return float(centers[best]), float(snr_db)


def tone_envelopes(x, fs, center, shift, baud, rate, alpha=0.75, chunk=1 << 20):
    """Envelopes of the high and low tones, integrated over alpha bit, sampled at `rate`.

    alpha < 1: a shorter integration leaves out the start of each bit, where the
    multipath echo of the previous bit lands (measured on DDH7, 2026-11-11).
    """
    L = max(1, int(round(alpha * fs / baud)))
    out = {f: [] for f in (center + shift / 2, center - shift / 2)}
    hist = {f: np.zeros(L, np.complex128) for f in out}
    for start in range(0, len(x), chunk):
        seg = x[start:start + chunk].astype(np.float64)
        n = np.arange(start, start + len(seg))
        pick = np.arange(np.ceil(start * rate / fs), (start + len(seg)) * rate / fs)
        pick = np.round(pick * fs / rate).astype(int) - start
        pick = pick[(pick >= 0) & (pick < len(seg))]
        for f in out:
            z = seg * np.exp(-2j * np.pi * f * n / fs)
            buf = np.concatenate([hist[f], z])
            c = np.concatenate([[0], np.cumsum(buf)])
            out[f].append(np.abs(c[L + 1:] - c[1:len(seg) + 1])[pick].astype(np.float32))
            hist[f] = buf[-L:]
    hi, lo = (np.concatenate(v) for v in out.values())
    return hi, lo


def peak_track(e, rate, window_s=0.3, block_s=0.05):
    """Running peak ("on" level) of an envelope: block maxima, then max over the window."""
    b = max(1, int(rate * block_s))
    nb = -(-len(e) // b)
    blocks = np.pad(e, (0, nb * b - len(e)), mode="edge").reshape(nb, b).max(axis=1)
    w = max(1, int(round(window_s / block_s)))
    padded = np.pad(blocks, (w // 2, w - w // 2 - 1), mode="edge")
    peaks = np.lib.stride_tricks.sliding_window_view(padded, w).max(axis=1)
    return np.repeat(peaks, b)[:len(e)] + 1e-9


def discriminator(hi, lo, rate, atc=True):
    """Decision variable in [-1, 1], positive on the higher tone.

    atc: automatic threshold correction, each tone normalised by its own running peak,
    so a selective fade of one tone does not move the threshold.
    """
    if atc:
        return np.clip(hi / peak_track(hi, rate), 0, 1) - np.clip(lo / peak_track(lo, rate), 0, 1)
    return (hi - lo) / (hi + lo + 1e-12)


def slice_uart(d, spb, squelch):
    """Asynchronous slicer on d (>0 = mark). Returns (index, code) list and error count."""
    c = np.concatenate([[0.0], np.cumsum(d, dtype=np.float64)])

    def bit(i, k):  # mean over the central 30 % of bit k
        a, b = int(i + (k + 0.35) * spb), int(i + (k + 0.65) * spb) + 1
        return (c[b] - c[a]) / max(1, b - a)

    starts = np.flatnonzero((d[:-1] > 0) & (d[1:] <= 0)) + 1
    chars, errors, nxt, end = [], 0, 0, len(d) - int(7.5 * spb)
    for i in starts:
        if i < nxt or i >= end:
            continue
        b = [bit(i, k) for k in range(7)]
        if b[0] >= 0 or np.mean(np.abs(b)) < squelch:
            continue
        if b[6] <= 0:  # no stop bit: framing error, keep searching
            errors += 1
            continue
        chars.append((int(i), sum(1 << k for k in range(5) if b[k + 1] > 0)))
        nxt = i + int(7 * spb)
    return chars, errors


def refine_shift(x, fs, center, shift, span=15):
    """Fine search of the shift around the nominal value (transmitters drift a few Hz)."""
    nfft = 1 << 15  # ~0.25 Hz bins at 8 kHz
    if len(x) < 2 * nfft:
        return center, shift
    w = np.hanning(nfft).astype(np.float32)
    starts = np.linspace(0, len(x) - nfft, min(200, len(x) // nfft)).astype(int)
    p = sum(np.abs(np.fft.rfft(x[i:i + nfft] * w)) ** 2 for i in starts)
    df = fs / nfft
    k = max(1, int(round(3 / df)))  # +-3 Hz
    p = np.convolve(p, np.ones(2 * k + 1), "same")
    f = np.arange(len(p)) * df
    s = np.arange(shift - span, shift + span + 0.01, 0.5)[:, None]
    c = np.arange(center - 30, center + 30.01, 0.5)[None, :]
    score = np.minimum(np.interp(c + s / 2, f, p), np.interp(c - s / 2, f, p))
    i, j = np.unravel_index(np.argmax(score), score.shape)
    return float(c[0, j]), float(s[i, 0])


def decode(path, shift=450.0, baud=50.0, center=None, polarity="auto",
           squelch=0.3, usos=False, alpha=0.75, atc=True):
    x, fs = read_decimated(path)
    snr = float("nan")
    if center is None:
        center, snr = find_center(x, fs, shift)
        center, shift = refine_shift(x, fs, center, shift)
    spb, rate = 20, baud * 20  # discriminator at 20 samples per bit
    hi, lo = tone_envelopes(x, fs, center, shift, baud, rate, alpha)
    d = discriminator(hi, lo, rate, atc)
    tries = {"high": d, "low": -d} if polarity == "auto" else {polarity: d if polarity == "high" else -d}
    best = None
    for name, dd in tries.items():
        chars, err = slice_uart(dd, spb, squelch)
        score = len(chars) - 2 * err
        if best is None or score > best[0]:
            best = (score, name, chars, err)
    _, mark, chars, err = best
    return {
        "fs": fs, "center_hz": center, "shift_hz": shift, "baud": baud,
        "mark": mark, "tone_snr_db": snr, "chars": len(chars), "framing_errors": err,
        "items": [(i / rate, code) for i, code in chars], "usos": usos,
    }


def render(result, start_utc=None, timestamps=False, gap_s=2.0):
    """Text output; with timestamps, one prefix per line (UTC if start time known)."""
    items = result["items"]
    if not timestamps:
        return ita2_decode([c for _, c in items], result["usos"])
    lines, cur, cur_t, last_t = [], [], None, None
    for t, code in items:
        if last_t is not None and t - last_t > gap_s and cur:
            cur.append(LETTERS.index("\n"))  # silence: break the line
        if cur_t is None:
            cur_t = t
        cur.append(code)
        last_t = t
    text = ita2_decode(cur, result["usos"])
    # Timestamps are approximate per line: spread time over characters.
    times = [t for t, c in items if c not in (LTRS, FIGS)]
    out, pos = [], 0
    for line in text.split("\n"):
        t = times[min(pos, len(times) - 1)] if times else 0
        stamp = (start_utc + dt.timedelta(seconds=t)).strftime("%H:%M:%S") if start_utc \
            else f"+{int(t) // 60:02d}:{int(t) % 60:02d}"
        if line.strip():
            out.append(f"[{stamp}] {line}")
        pos += len(line) + 1
    return "\n".join(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("wav")
    p.add_argument("--shift", type=float, default=450)
    p.add_argument("--baud", type=float, default=50)
    p.add_argument("--center", type=float, help="audio center frequency (Hz); default: search")
    p.add_argument("--mark", choices=["auto", "high", "low"], default="auto")
    p.add_argument("--squelch", type=float, default=0.3)
    p.add_argument("--usos", action="store_true", help="unshift on space")
    p.add_argument("--alpha", type=float, default=0.75, help="integration length (bits)")
    p.add_argument("--no-atc", action="store_true", help="plain (hi-lo)/(hi+lo) discriminator")
    p.add_argument("--timestamps", action="store_true")
    p.add_argument("-o", "--out", help="text output file (default stdout)")
    a = p.parse_args(argv)

    r = decode(a.wav, a.shift, a.baud, a.center, a.mark, a.squelch, a.usos, a.alpha,
               not a.no_atc)
    start = None
    meta = pathlib.Path(a.wav).with_suffix(".json")
    if meta.exists():
        try:
            s = json.loads(meta.read_text())["start_utc"]
            start = dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ")
        except (KeyError, ValueError):
            pass
    text = render(r, start, a.timestamps)
    if a.out:
        pathlib.Path(a.out).write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.write(text + "\n")
    rate = r["framing_errors"] / max(1, r["chars"] + r["framing_errors"])
    print(f"center {r['center_hz']:.1f} Hz, shift {r['shift_hz']:.1f} Hz, mark {r['mark']}, "
          f"tone SNR {r['tone_snr_db']:.1f} dB, {r['chars']} chars, "
          f"{r['framing_errors']} framing errors ({100 * rate:.1f} %)", file=sys.stderr)


if __name__ == "__main__":
    main()
