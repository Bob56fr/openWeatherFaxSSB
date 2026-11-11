// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Web interface of owfaxd (spec ARC-002/003, UI-001/005/006/008/009, DSP-007, NOT-005).
// Plain ES2017, no build step, no dependency: works offline on old browsers.
// Every text that comes from the data is inserted with textContent, never as HTML.

'use strict';

(function () {
  const API = '/api/v1';
  const state = {
    i18n: {},
    lang: 'fr',
    status: null,
    ws: null,
    wsDelay: 1000,
    live: { stem: '', width: 0, rows: 0, firstLine: -1, refill: null },
    chart: { data: null, version: 'raw', light: true, fit: true },
    scheduleTimer: null,
  };

  // ------------------------------------------------------------------ helpers

  function $(id) { return document.getElementById(id); }

  function store(key, value) {
    try { window.localStorage.setItem('owfax.' + key, value); } catch (e) { /* private mode */ }
  }

  function load(key, fallback) {
    try {
      const v = window.localStorage.getItem('owfax.' + key);
      return v === null ? fallback : v;
    } catch (e) {
      return fallback;
    }
  }

  function t(key, vars) {
    let s = Object.prototype.hasOwnProperty.call(state.i18n, key) ? state.i18n[key] : key;
    if (vars) {
      Object.keys(vars).forEach(function (k) { s = s.split('{' + k + '}').join(String(vars[k])); });
    }
    return s;
  }

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }

  async function getJSON(url) {
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error(url + ': ' + r.status);
    return r.json();
  }

  function chartUrl(stem, version, light, width) {
    let q = [];
    if (light) q.push('levels=16');
    if (width) q.push('width=' + width);
    return API + '/charts/' + encodeURIComponent(stem) + '/' + version + '.png' +
      (q.length ? '?' + q.join('&') : '');
  }

  /** "20261110T0735Z" → "2026-11-10 07:35 UTC" */
  function when(stamp) {
    const m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})Z$/.exec(stamp || '');
    if (!m) return stamp || '';
    return m[1] + '-' + m[2] + '-' + m[3] + ' ' + m[4] + ':' + m[5] + ' UTC';
  }

  function duration(seconds) {
    const s = Math.round(seconds || 0);
    const m = Math.floor(s / 60);
    return m > 0 ? m + ' min ' + String(s % 60).padStart(2, '0') + ' s' : s + ' s';
  }

  function number(v, digits) {
    return Number(v || 0).toLocaleString(state.lang, {
      minimumFractionDigits: digits, maximumFractionDigits: digits,
    });
  }

  function signed(v, digits) {
    const n = Number(v || 0);
    return (n >= 0 ? '+' : '−') + number(Math.abs(n), digits);
  }

  // ------------------------------------------------------------ i18n, themes

  async function setLanguage(lang) {
    state.lang = lang === 'en' ? 'en' : 'fr';
    store('lang', state.lang);
    try {
      state.i18n = await getJSON('i18n/' + state.lang + '.json');
    } catch (e) {
      state.i18n = {};
    }
    document.documentElement.lang = state.lang;
    document.querySelectorAll('[data-i18n]').forEach(function (n) {
      n.textContent = t(n.getAttribute('data-i18n'));
    });
    pressed('[data-lang]', 'data-lang', state.lang);
    renderBadge();
    route();
  }

  function setTheme(theme) {
    const root = document.documentElement;
    if (theme === 'auto') root.removeAttribute('data-theme');
    else root.setAttribute('data-theme', theme);
    store('theme', theme);
    pressed('[data-theme-choice]', 'data-theme-choice', theme);
  }

  function setSound(on) {
    store('sound', on ? 'on' : 'off');
    pressed('[data-sound]', 'data-sound', on ? 'on' : 'off');
  }

  function pressed(selector, attr, value) {
    document.querySelectorAll(selector).forEach(function (b) {
      b.setAttribute('aria-pressed', b.getAttribute(attr) === value ? 'true' : 'false');
    });
  }

  // ------------------------------------------------------------ alerts (NOT-005)

  let toastTimer = null;
  function toast(text) {
    const n = $('toast');
    n.textContent = text;
    n.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { n.hidden = true; }, 6000);
    if (load('sound', 'off') === 'on') beep();
  }

  function beep() {
    try {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return;
      const ctx = new Ctx();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.frequency.value = 880;
      gain.gain.value = 0.15;
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.start();
      osc.stop(ctx.currentTime + 0.35);
      osc.onended = function () { ctx.close(); };
    } catch (e) { /* no audio */ }
  }

  // ------------------------------------------------------------------ status

  /** "20261110T2343Z_GYA_rejeu" → "GYA rejeu" */
  function stemLabel(stem) {
    return String(stem || '').split('_').slice(1).join(' ');
  }

  /**
   * Header badge: engine state and, during a reception, its step (waiting for the
   * phasing, decoding with the line count). Blinks while a chart is being received.
   */
  function renderBadge() {
    const b = $('state');
    const s = state.status;
    const st = s ? s.state : 'offline';
    let text = t('state.' + st);
    let cls = st;
    if (state.done && Date.now() < state.done.until) {
      cls = 'done';
      text = t('phase.done', { label: stemLabel(state.done.stem) });
    } else if (st === 'listening') {
      text += ' · ' + t('phase.waiting');
    } else if (st === 'receiving') {
      const phase = s.phase || 'image';
      const label = stemLabel(s.current);
      if (phase === 'phasing') {
        text += ' · ' + label + ' · ' + t(s.start === 'signal' ? 'phase.signal' : 'phase.phasing');
      } else {
        text += ' · ' + label + ' · ' + t('phase.image', { lines: Math.max(s.lines || 0, state.live.rows) });
      }
      cls += ' blink';
    }
    b.className = 'badge ' + cls;
    b.textContent = text;
  }

  // ------------------------------------------------------ live view (DSP-007)

  function liveCanvas() { return $('live-canvas'); }

  /** Makes room for `rows` lines of `width` pixels, keeping what is drawn. */
  function ensureCanvas(width, rows) {
    const c = liveCanvas();
    if (c.width === width && c.height >= rows) return;
    const keep = document.createElement('canvas');
    keep.width = c.width;
    keep.height = c.height;
    keep.getContext('2d').drawImage(c, 0, 0);
    const sameWidth = c.width === width;
    c.width = width;
    c.height = Math.max(rows, sameWidth ? c.height * 2 : rows, 64);
    const ctx = c.getContext('2d');
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, c.width, c.height);
    if (sameWidth) ctx.drawImage(keep, 0, 0);
  }

  function resetLive(stem, ioc) {
    state.live = { stem: stem, width: ioc === 288 ? 905 : 1810, rows: 0, firstLine: -1, refill: null };
    const c = liveCanvas();
    c.width = state.live.width;
    c.height = 1;
    ensureCanvas(state.live.width, 64);

  }

  /** Draws what was received before this page connected (live preview, 16 levels). */
  function loadLiveBase(stem) {
    const img = new Image();
    img.onload = function () {
      if (state.live.stem !== stem) return;
      ensureCanvas(img.naturalWidth, img.naturalHeight + 1);
      liveCanvas().getContext('2d').drawImage(img, 0, 0);
      state.live.rows = Math.max(state.live.rows, img.naturalHeight);
      fitLiveHeight();
      renderLiveInfo();
    };
    img.src = chartUrl(stem, 'live', true) + '&t=' + Date.now();
  }

  function fitLiveHeight() {
    // The canvas is allocated ahead (doubling); its wrapper shows the received rows only.
    const c = liveCanvas();
    const wrap = c.parentNode;
    const w = wrap.clientWidth || c.width;
    wrap.style.height = Math.max(1, Math.round(w * Math.max(1, state.live.rows) / c.width)) + 'px';
  }

  function drawLine(m) {
    if (state.live.stem !== m.stem) resetLive(m.stem, m.width === 905 ? 288 : 576);
    const width = m.width;
    const bin = atob(m.pixels);
    ensureCanvas(width, m.index + 1);
    const ctx = liveCanvas().getContext('2d');
    const row = ctx.createImageData(width, 1);
    for (let x = 0; x < width && x < bin.length; ++x) {
      const v = bin.charCodeAt(x);
      row.data[4 * x] = v;
      row.data[4 * x + 1] = v;
      row.data[4 * x + 2] = v;
      row.data[4 * x + 3] = 255;
    }
    ctx.putImageData(row, 0, m.index);
    if (state.live.firstLine < 0) {
      state.live.firstLine = m.index;
      if (m.index > state.live.rows + 1) {
        // Lines between the last saved preview and this page's connection: refill
        // them from the preview once it has been rewritten (every 50 lines).
        state.live.refill = setTimeout(function () { loadLiveBase(m.stem); }, 30000);
      }
    }
    state.live.rows = Math.max(state.live.rows, m.index + 1);
    if (m.index % 10 === 0) fitLiveHeight();
    if (m.index % 20 === 0) renderLiveInfo();
    if (m.index % 10 === 0) renderBadge();
  }

  function renderLiveInfo(extra) {
    const s = state.status;
    const receiving = s && s.state === 'receiving' && s.current;
    $('live-idle').hidden = !!receiving;
    const st = s ? s.state : 'offline';
    $('live-idle').textContent = t(st === 'stopped' ? 'live.stopped' : st === 'offline' ? 'live.offline' : 'live.idle');
    liveCanvas().parentNode.hidden = !receiving;
    if (receiving) {
      const stamp = s.current.split('_')[0];
      let text = t('live.receiving', { label: s.current.split('_').slice(1).join(' '), when: when(stamp), lines: Math.max(s.lines || 0, state.live.rows) });
      if (state.live.locked) text += ' — ' + state.live.locked;
      if (extra) text += ' — ' + extra;
      $('live-info').textContent = text;
    } else {
      $('live-info').textContent = '';
    }
  }

  async function renderLastChart() {
    try {
      const list = await getJSON(API + '/charts?limit=1');
      const a = $('live-last');
      if (list.length) {
        a.textContent = t('live.last', {
          when: when(list[0].start_utc) + ' · ' + String(list[0].label || '').split('_').join(' '),
        });
        a.href = '#chart/' + encodeURIComponent(list[0].stem);
        a.hidden = false;
      } else {
        a.hidden = true;
      }
    } catch (e) { /* offline */ }
  }

  function onMessage(m) {
    if (m.type === 'spectrum') {
      onSpectrum(m);
    } else if (m.type === 'status') {
      const before = state.status;
      state.status = m.status;
      renderBadge();
      if (m.status.state === 'receiving' && m.status.current && state.live.stem !== m.status.current) {
        resetLive(m.status.current, 576);
        loadLiveBase(m.status.current);
      }
      if (!before || before.state !== m.status.state) renderLiveInfo();
    } else if (m.type === 'start') {
      resetLive(m.stem, m.ioc);
      state.live.locked = t('start.' + m.start);
      state.done = null;
      if (state.status) {
        state.status.state = 'receiving';
        state.status.current = m.stem;
        state.status.phase = 'phasing';
        state.status.start = m.start;
        state.status.lines = 0;
      }
      renderBadge();
      renderLiveInfo();
      toast(t('toast.new_start', { label: m.stem }));
    } else if (m.type === 'lock') {
      state.live.locked = t('live.locked', { ppm: signed(m.clock_ppm, 1), hz: signed(m.tuning_offset_hz, 0) }) +
        ', ' + t(m.phasing_found ? 'live.phasing' : 'live.no_phasing');
      if (state.status) state.status.phase = 'image';
      renderBadge();
      renderLiveInfo();
    } else if (m.type === 'line') {
      drawLine(m);
    } else if (m.type === 'complete') {
      if (state.status) {
        state.status.state = 'listening';
        state.status.current = '';
        state.status.phase = 'listening';
      }
      if (!m.discarded) {
        // "Chart received" for a few seconds, then back to "listening".
        state.done = { stem: m.stem, until: Date.now() + 15000 };
        setTimeout(renderBadge, 15100);
      }
      renderBadge();
      renderLiveInfo();
      renderLastChart();
      if (!m.discarded) {
        toast(t('toast.new_chart', { label: m.stem }));
        if (currentView() === 'charts') renderCharts();
      }
    }
  }

  function connect() {
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + API + '/live';
    let ws;
    try { ws = new WebSocket(url); } catch (e) { return retry(); }
    state.ws = ws;
    ws.onopen = function () { state.wsDelay = 1000; };
    ws.onmessage = function (ev) {
      try { onMessage(JSON.parse(ev.data)); } catch (e) { /* ignore a bad message */ }
    };
    ws.onclose = function () {
      state.status = null;
      renderBadge();
      retry();
    };
  }

  function retry() {
    setTimeout(connect, state.wsDelay);
    state.wsDelay = Math.min(30000, state.wsDelay * 2);
  }

  // ------------------------------------------------------------------ gallery

  async function renderCharts() {
    const ul = $('gallery');
    let list = [];
    try { list = await getJSON(API + '/charts?limit=60'); } catch (e) { return; }
    ul.textContent = '';
    $('charts-empty').hidden = list.length > 0;
    list.forEach(function (c) {
      if (!c.lines) return;
      const li = el('li');
      const a = el('a');
      a.href = '#chart/' + encodeURIComponent(c.stem);
      const img = el('img');
      img.loading = 'lazy';
      img.alt = c.stem;
      img.src = chartUrl(c.stem, 'raw', true, 360);
      img.srcset = chartUrl(c.stem, 'raw', true, 360) + ' 1x, ' + chartUrl(c.stem, 'raw', true, 720) + ' 2x';
      a.appendChild(img);
      const cap = el('div', 'caption');
      cap.appendChild(el('div', 'when', when(c.start_utc)));
      cap.appendChild(el('div', '', String(c.label || '').split('_').join(' ')));
      cap.appendChild(el('div', 'facts', t('charts.lines', { lines: c.lines }) + ' · ' +
        t('charts.quality', { q: Math.round((c.quality || 0) * 100) }) +
        (c.start && c.start !== 'apt' ? ' · ' + t('start.' + c.start) : '')));
      a.appendChild(cap);
      li.appendChild(a);
      ul.appendChild(li);
    });
  }

  // ------------------------------------------------- one chart (UI-008/009)

  async function renderChart(stem) {
    let c;
    try { c = await getJSON(API + '/charts/' + encodeURIComponent(stem)); } catch (e) {
      $('chart-title').textContent = stem;
      return;
    }
    state.chart.data = c;
    const hasEnhanced = c.lines > 0 && c.enhancements && c.enhancements.length > 0;
    const available = { raw: c.lines > 0, enhanced: hasEnhanced, live: c.live_lines > 0 };
    if (!available[state.chart.version]) state.chart.version = available.raw ? 'raw' : 'live';
    document.querySelectorAll('[data-version]').forEach(function (b) {
      b.disabled = !available[b.getAttribute('data-version')];
    });
    $('chart-title').textContent = when(c.start_utc) + ' — ' + String(c.label || '').split('_').join(' ');
    $('chart-download').href = chartUrl(c.stem, 'raw', false);
    $('chart-download').hidden = !available.raw;

    const rows = [
      ['report.start', when(c.start_utc)],
      ['report.duration', duration(c.duration_s)],
      ['report.start_kind', t('start.' + (c.start || (c.apt_start ? 'apt' : 'forced')))],
      ['report.lines', String(c.lines)],
      ['report.ioc', String(c.ioc)],
      ['report.phasing', t(c.phasing_found ? 'yes' : 'no')],
      ['report.clock', signed(c.clock_ppm, 1) + ' ppm'],
      ['report.tuning', signed(c.tuning_offset_hz, 1) + ' Hz'],
      ['report.quality', Math.round((c.quality || 0) * 100) + ' %'],
      ['report.enhancements', hasEnhanced ? c.enhancements.map(function (e) {
        return e === 'clicks' ? t('report.clicks', { n: c.clicks_repaired || 0 }) : e;
      }).join(', ') : t('report.none')],
    ];
    const table = $('chart-report');
    table.textContent = '';
    rows.forEach(function (r) {
      const tr = el('tr');
      tr.appendChild(el('th', '', t(r[0])));
      tr.appendChild(el('td', '', r[1]));
      table.appendChild(tr);
    });
    showChartImage();
  }

  function showChartImage() {
    const c = state.chart.data;
    if (!c) return;
    const frame = $('chart-frame');
    // Same place, same size: switching raw / enhanced compares them directly (UI-009).
    $('chart-img').src = chartUrl(c.stem, state.chart.version, state.chart.light);
    $('chart-img').alt = c.stem + ' (' + t('chart.' + state.chart.version) + ')';
    frame.className = 'chart-frame' + (state.chart.fit ? ' fit' : '');
    pressed('[data-version]', 'data-version', state.chart.version);
    $('quality-toggle').textContent = state.chart.light ? t('chart.light') : t('chart.full');
    $('quality-toggle').setAttribute('aria-pressed', state.chart.light ? 'false' : 'true');
    $('zoom-toggle').textContent = state.chart.fit ? t('chart.fit') : t('chart.actual');
    $('zoom-toggle').setAttribute('aria-pressed', state.chart.fit ? 'false' : 'true');
  }

  // ------------------------------------------------------------------ schedule

  function minutesOf(hhmm) {
    const m = /^(\d{1,2}):(\d{2})/.exec(hhmm || '');
    return m ? Number(m[1]) * 60 + Number(m[2]) : -1;
  }

  function untilText(minutes) {
    const h = Math.floor(minutes / 60);
    const m = minutes % 60;
    return t('schedule.in', { d: (h > 0 ? h + ' h ' + String(m).padStart(2, '0') : m + ' min') });
  }

  async function renderSchedule() {
    const box = $('schedule');
    const now = new Date();
    const nowMin = now.getUTCHours() * 60 + now.getUTCMinutes();
    $('schedule-clock').textContent = t('schedule.now', {
      time: String(now.getUTCHours()).padStart(2, '0') + ':' + String(now.getUTCMinutes()).padStart(2, '0'),
    });
    let stations;
    try { stations = await getJSON(API + '/stations'); } catch (e) {
      box.textContent = '';
      box.appendChild(el('p', 'empty', t('schedule.empty')));
      return;
    }
    box.textContent = '';
    stations.forEach(function (s) {
      const sec = el('section', 'station');
      sec.appendChild(el('h2', '', s.name || s.id));
      const freqs = ((s.wefax && s.wefax.frequencies) || []).map(function (f) {
        return (f.callsign ? f.callsign + ' ' : '') + number(f.assigned_khz, f.assigned_khz % 1 ? 1 : 0) + ' kHz';
      });
      if (freqs.length) sec.appendChild(el('p', 'meta', t('schedule.frequencies') + ' : ' + freqs.join(' · ')));
      // The next 24 hours, starting with the next transmission (UTC wraps at midnight).
      const slots = (s.schedule || []).filter(function (x) { return minutesOf(x.utc) >= 0; })
        .map(function (x) { return { slot: x, wait: (minutesOf(x.utc) - nowMin + 1440) % 1440 }; })
        .sort(function (a, b) { return a.wait - b.wait; });
      if (!slots.length) {
        sec.appendChild(el('p', 'empty', t('schedule.no_schedule')));
      } else {
        const table = el('table', 'slots');
        slots.forEach(function (x, i) {
          const tr = el('tr', i === 0 ? 'next' : '');
          tr.appendChild(el('td', 'time', x.slot.utc));
          tr.appendChild(el('td', '', x.slot.content || ''));
          tr.appendChild(el('td', 'in', untilText(x.wait)));
          table.appendChild(tr);
        });
        sec.appendChild(table);
      }
      box.appendChild(sec);
    });
  }

  // ------------------------------------------------------- diagnostic (UI-007)

  const diag = { shift: 800, palette: null };

  function palette() {
    // Waterfall colours: dark blue → cyan → yellow → white.
    if (diag.palette) return diag.palette;
    const stops = [[11, 15, 20], [20, 40, 120], [0, 170, 200], [240, 220, 40], [255, 255, 255]];
    const p = [];
    for (let i = 0; i < 256; ++i) {
      const x = (i / 255) * (stops.length - 1);
      const k = Math.min(stops.length - 2, Math.floor(x));
      const f = x - k;
      p.push(stops[k].map(function (c, j) { return Math.round(c + (stops[k + 1][j] - c) * f); }));
    }
    diag.palette = p;
    return p;
  }

  function hzToX(hz, maxHz, width) { return (hz / maxHz) * width; }

  /** Level meter and advice (UI-003 audio level check); `prefix` selects the meter. */
  function renderLevel(m, prefix) {
    const pct = function (db) { return Math.max(0, Math.min(100, (db + 60) / 60 * 100)); };
    const rms = $(prefix + 'meter-rms');
    rms.style.width = pct(m.rms_dbfs) + '%';
    $(prefix + 'meter-peak').style.left = 'calc(' + pct(m.peak_dbfs) + '% - 3px)';
    let advice = 'diag.level_ok';
    rms.className = '';
    if (m.clipped > 0 || m.peak_dbfs > -1) { advice = 'diag.level_clip'; rms.className = 'high'; }
    else if (m.rms_dbfs < -50) { advice = 'diag.level_low'; rms.className = 'low'; }
    $(prefix + 'level-text').textContent = t('diag.level_value', {
      rms: number(m.rms_dbfs, 1), peak: number(m.peak_dbfs, 1),
    }) + ' — ' + t(advice);
  }

  function onSpectrum(m) {
    if (currentView() === 'setup') renderLevel(m, 'setup-');
    if (currentView() !== 'diag') return;
    $('spectrum-wait').hidden = true;
    const bin = atob(m.bands);
    const n = bin.length;
    const span = m.db_max - m.db_min;
    renderLevel(m, '');

    // Spectrum.
    const sc = $('spectrum');
    const ctx = sc.getContext('2d');
    const w = sc.width;
    const h = sc.height;
    ctx.fillStyle = '#0b0f14';
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = '#2c3945';
    ctx.fillStyle = '#9aabb8';
    ctx.font = '11px sans-serif';
    ctx.lineWidth = 1;
    for (let hz = 500; hz < m.max_hz; hz += 500) {
      const x = Math.round(hzToX(hz, m.max_hz, w)) + 0.5;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
      ctx.fillText(String(hz), x + 3, h - 4);
    }
    // Expected black, centre and white of the station (a mistuned SSB shows shifted peaks).
    const marks = [[1900 - diag.shift / 2, '#7f8c99'], [1900, '#556270'], [1900 + diag.shift / 2, '#e4ebf0']];
    ctx.setLineDash([4, 4]);
    marks.forEach(function (mk) {
      const x = Math.round(hzToX(mk[0], m.max_hz, w)) + 0.5;
      ctx.strokeStyle = mk[1];
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    });
    ctx.setLineDash([]);
    ctx.strokeStyle = '#5cc27e';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let i = 0; i < n; ++i) {
      const x = (i + 0.5) / n * w;
      const y = h - (bin.charCodeAt(i) / 255) * h;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.fillStyle = '#9aabb8';
    ctx.fillText(m.db_max + ' dB', 4, 12);
    ctx.fillText((m.db_min + span / 2) + ' dB', 4, h / 2);

    // Waterfall: newest line on top.
    const wf = $('waterfall');
    const wctx = wf.getContext('2d');
    wctx.drawImage(wf, 0, 0, wf.width, wf.height - 1, 0, 1, wf.width, wf.height - 1);
    const row = wctx.createImageData(wf.width, 1);
    const pal = palette();
    for (let x = 0; x < wf.width; ++x) {
      const c = pal[bin.charCodeAt(Math.min(n - 1, Math.floor(x / wf.width * n)))];
      row.data[4 * x] = c[0];
      row.data[4 * x + 1] = c[1];
      row.data[4 * x + 2] = c[2];
      row.data[4 * x + 3] = 255;
    }
    wctx.putImageData(row, 0, 0);
  }

  async function renderDiag() {
    const table = $('diag-table');
    let info = {};
    let d = {};
    try { info = await getJSON(API + '/info'); } catch (e) { /* offline */ }
    try { d = await getJSON(API + '/diagnostics'); } catch (e) { /* older engine */ }
    const host = d.host || {};
    const input = d.input || {};
    const opt = d.options || {};
    if (opt.shift_hz) diag.shift = opt.shift_hz;
    const s = state.status || {};
    const gib = host.ram_bytes ? number(host.ram_bytes / 1073741824, 1) + ' Gio' : '?';
    const rows = [
      ['diag.version', (info.version || '?') + ' (' + (info.git_revision || '?') + ')'],
      ['diag.os', (host.os || '?') + ', ' + (host.cpu_cores || '?') + ' ' + t('diag.cores') + ', ' + gib],
      ['diag.tier', host.tier ? t('tier.' + host.tier) : '?'],
      ['diag.input', (input.description || info.input || '?')],
      ['diag.backends', (d.backends || []).join(', ')],
      ['diag.devices', (d.inputs || []).map(function (x) { return x.uri; }).join('\n') || '—'],
      ['diag.shift', (opt.shift_hz || '?') + ' Hz'],
      ['diag.auto_start', t(opt.start_without_apt ? 'yes' : 'no')],
      ['diag.overruns', String(s.overruns === undefined ? '?' : s.overruns) + ' / ' +
        String(s.dropped_samples === undefined ? '?' : s.dropped_samples)],
      ['diag.clients', String(s.live_clients === undefined ? '?' : s.live_clients)],
    ];
    table.textContent = '';
    rows.forEach(function (r) {
      const tr = el('tr');
      tr.appendChild(el('th', '', t(r[0])));
      const td = el('td', '', r[1]);
      td.style.whiteSpace = 'pre-line';
      tr.appendChild(td);
      table.appendChild(tr);
    });
  }

  // --------------------------------------- skipper settings (UI-002, UI-004)

  const skipper = { version: 0, token: '' };

  function token() {
    try { return window.sessionStorage.getItem('owfax.token') || ''; } catch (e) { return skipper.token; }
  }

  function setToken(v) {
    skipper.token = v || '';
    try {
      if (v) window.sessionStorage.setItem('owfax.token', v);
      else window.sessionStorage.removeItem('owfax.token');
    } catch (e) { /* private mode: kept in memory */ }
  }

  async function call(method, path, body) {
    const headers = { 'Content-Type': 'application/json' };
    if (token()) headers.Authorization = 'Bearer ' + token();
    const r = await fetch(API + path, {
      method: method, headers: headers, cache: 'no-store',
      body: body === undefined ? (method === 'GET' ? undefined : '{}') : JSON.stringify(body),
    });
    let data = {};
    try { data = await r.json(); } catch (e) { /* empty body */ }
    return { status: r.status, data: data };
  }

  function message(key, vars, isError) {
    const m = $('skipper-message');
    m.textContent = t(key, vars);
    m.className = 'notice' + (isError ? ' error' : '');
    m.hidden = false;
  }

  function showForms(which) {
    ['form-create', 'form-login', 'form-settings', 'form-password'].forEach(function (id) {
      $(id).hidden = which.indexOf(id) < 0;
    });
  }

  async function renderSkipper() {
    let r;
    try { r = await call('GET', '/settings'); } catch (e) { return; }
    $('skipper-unavailable').hidden = r.status !== 404;
    if (r.status === 404) { showForms([]); return; }
    const d = r.data;
    skipper.version = d.version;
    if (!d.password_set) { setToken(''); showForms(['form-create']); return; }
    if (!d.logged_in) { setToken(''); showForms(['form-login']); return; }
    showForms(['form-settings', 'form-password']);
    const f = $('form-settings');
    const s = d.settings || {};
    f.label.value = s.label || '';
    const shift = String(Math.round(s.shift_hz || 800));
    if (!Array.prototype.some.call(f.shift_hz.options, function (o) { return o.value === shift; })) {
      f.shift_hz.appendChild(new Option(shift + ' Hz', shift));
    }
    f.shift_hz.value = shift;
    f.start_without_apt.checked = !!s.start_without_apt;
    f.enhanced.checked = !!s.enhanced;
    // Audio inputs: the one in use, the one chosen, and the devices found (UI-007).
    let inputs = [];
    try { inputs = ((await getJSON(API + '/diagnostics')).inputs || []).map(function (x) { return x.uri; }); }
    catch (e) { /* none */ }
    const choices = [d.running_input, s.input].concat(inputs).filter(function (v, i, a) {
      return v && a.indexOf(v) === i;
    });
    f.input.textContent = '';
    choices.forEach(function (v) { f.input.appendChild(new Option(v, v)); });
    f.input.value = s.input || d.running_input;
    $('input-help').textContent = d.restart_required ? t('skipper.restart_needed') : t('skipper.input_help');
    $('restart').hidden = !d.restart_required;
  }

  async function onCreate(ev) {
    ev.preventDefault();
    const f = ev.target;
    if (f.new.value !== f.confirm.value) return message('skipper.mismatch', null, true);
    const r = await call('POST', '/password', { new: f.new.value });
    if (r.status !== 200) return message('skipper.error', { e: r.data.error || r.status }, true);
    setToken(r.data.token);
    f.reset();
    message('skipper.created');
    renderSkipper();
  }

  async function onLogin(ev) {
    ev.preventDefault();
    const f = ev.target;
    const r = await call('POST', '/session', { password: f.password.value });
    f.reset();
    if (r.status === 429) return message('skipper.locked', null, true);
    if (r.status !== 200) return message('skipper.wrong', null, true);
    setToken(r.data.token);
    $('skipper-message').hidden = true;
    renderSkipper();
  }

  async function onSave(ev) {
    ev.preventDefault();
    const f = ev.target;
    const settings = {
      label: f.label.value.trim(),
      shift_hz: Number(f.shift_hz.value),
      start_without_apt: f.start_without_apt.checked,
      enhanced: f.enhanced.checked,
      input: f.input.value,
    };
    const r = await call('PUT', '/settings', { version: skipper.version, settings: settings });
    if (r.status === 401) { setToken(''); renderSkipper(); return message('skipper.expired', null, true); }
    if (r.status === 409) { renderSkipper(); return message('skipper.conflict', null, true); }
    if (r.status !== 200) return message('skipper.error', { e: r.data.error || r.status }, true);
    message(r.data.restart_required ? 'skipper.saved_restart' : 'skipper.saved');
    renderSkipper();
  }

  async function onPassword(ev) {
    ev.preventDefault();
    const f = ev.target;
    if (f.new.value !== f.confirm.value) return message('skipper.mismatch', null, true);
    const r = await call('POST', '/password', { current: f.current.value, new: f.new.value });
    f.reset();
    if (r.status !== 200) {
      return message(r.status === 401 ? 'skipper.wrong' : 'skipper.error', { e: r.data.error || r.status }, true);
    }
    setToken(r.data.token);
    message('skipper.changed');
  }

  async function onLogout() {
    await call('DELETE', '/session');
    setToken('');
    $('skipper-message').hidden = true;
    renderSkipper();
  }

  // ------------------------------------------------- setup assistant (UI-003)

  const setup = { step: 1, last: 6, settings: null, version: 0, passwordSet: false, input: '', station: null };

  function setupMessage(key, vars, isError) {
    const m = $('setup-message');
    if (!key) { m.hidden = true; return; }
    m.textContent = t(key, vars);
    m.className = 'notice' + (isError ? ' error' : '');
    m.hidden = false;
  }

  async function setupLoad() {
    const r = await call('GET', '/settings');
    if (r.status === 404) {
      setupMessage('skipper.unavailable', null, true);
      return false;
    }
    setup.settings = r.data.settings || {};
    setup.version = r.data.version;
    setup.passwordSet = !!r.data.password_set;
    setup.loggedIn = !!r.data.logged_in;
    setup.running = r.data.running_input;
    if (!setup.passwordSet) setToken('');
    return true;
  }

  async function renderSetupStep() {
    document.querySelectorAll('.setup-step').forEach(function (d) {
      d.hidden = Number(d.getAttribute('data-step')) !== setup.step;
    });
    const steps = $('setup-steps');
    steps.textContent = '';
    for (let i = 1; i <= setup.last; ++i) {
      steps.appendChild(el('li', i === setup.step ? 'current' : (i < setup.step ? 'done' : ''), String(i)));
    }
    $('setup-back').disabled = setup.step === 1;
    $('setup-next').textContent = t(setup.step === setup.last ? 'setup.finish' : 'setup.next');
    pressed('[data-lang]', 'data-lang', state.lang);

    if (setup.step === 2) {
      const logged = setup.passwordSet && setup.loggedIn;
      $('setup-pw-text').textContent = t(!setup.passwordSet ? 'skipper.create_help'
        : logged ? 'setup.pw_done' : 'skipper.login_help');
      $('setup-pw-current').hidden = !setup.passwordSet || logged;
      $('setup-pw-new').hidden = setup.passwordSet;
      $('setup-pw-confirm').hidden = setup.passwordSet;
    } else if (setup.step === 3) {
      let inputs = [];
      try { inputs = ((await getJSON(API + '/diagnostics')).inputs || []).map(function (x) { return x.uri; }); }
      catch (e) { /* none */ }
      const sel = $('setup-input');
      const choices = [setup.input || setup.settings.input, setup.running].concat(inputs)
        .filter(function (v, i, a) { return v && a.indexOf(v) === i; });
      sel.textContent = '';
      choices.forEach(function (v) { sel.appendChild(new Option(v, v)); });
      sel.value = setup.input || setup.settings.input || setup.running;
      $('setup-input-note').textContent = t('setup.input_now', { input: setup.running || '?' });
    } else if (setup.step === 5) {
      const box = $('setup-stations');
      box.textContent = '';
      let stations = [];
      try { stations = await getJSON(API + '/stations'); } catch (e) { /* no catalogue */ }
      if (!stations.length) box.appendChild(el('p', 'empty', t('schedule.empty')));
      const current = setup.station ? setup.station.id : setup.settings.station;
      stations.forEach(function (s) {
        const label = el('label', 'check');
        const radio = el('input');
        radio.type = 'radio';
        radio.name = 'station';
        radio.value = s.id;
        radio.checked = s.id === current;
        radio.addEventListener('change', function () { setup.station = s; });
        if (radio.checked) setup.station = s;
        label.appendChild(radio);
        const w = s.wefax || {};
        const freqs = (w.frequencies || []).map(function (f) { return number(f.assigned_khz, f.assigned_khz % 1 ? 1 : 0); });
        label.appendChild(el('span', '', (s.name || s.id) + ' — ' + freqs.join(' / ') + ' kHz — ' +
          t('skipper.shift').toLowerCase() + ' ' + (w.shift_hz || 800) + ' Hz'));
        box.appendChild(label);
      });
    } else if (setup.step === 6) {
      const table = $('setup-summary');
      table.textContent = '';
      const st = setup.station;
      [
        ['settings.language', state.lang === 'fr' ? 'Français' : 'English'],
        ['setup.s2', t(setup.passwordSet ? 'yes' : 'no')],
        ['skipper.input', setup.input || setup.settings.input || setup.running || '?'],
        ['setup.s5', st ? (st.name || st.id) : '—'],
        ['skipper.shift', (st && st.wefax && st.wefax.shift_hz ? st.wefax.shift_hz : setup.settings.shift_hz) + ' Hz'],
      ].forEach(function (r) {
        const tr = el('tr');
        tr.appendChild(el('th', '', t(r[0])));
        tr.appendChild(el('td', '', r[1]));
        table.appendChild(tr);
      });
    }
  }

  /** Validates the current step; true to go on. */
  async function setupLeave() {
    setupMessage(null);
    if (setup.step === 2) {
      const f = $('setup-pw');
      if (!setup.passwordSet) {
        if (f.new.value.length < 8) { setupMessage('setup.pw_short', null, true); return false; }
        if (f.new.value !== f.confirm.value) { setupMessage('skipper.mismatch', null, true); return false; }
        const r = await call('POST', '/password', { new: f.new.value });
        if (r.status !== 200) { setupMessage('skipper.error', { e: r.data.error || r.status }, true); return false; }
        setToken(r.data.token);
      } else if (!setup.loggedIn) {
        const r = await call('POST', '/session', { password: f.current.value });
        if (r.status === 429) { setupMessage('skipper.locked', null, true); return false; }
        if (r.status !== 200) { setupMessage('skipper.wrong', null, true); return false; }
        setToken(r.data.token);
      }
      f.reset();
      await setupLoad();
      return setup.loggedIn;
    }
    if (setup.step === 3) setup.input = $('setup-input').value;
    if (setup.step === 6) {
      const st = setup.station;
      const s = { input: setup.input || setup.settings.input };
      if (st) {
        s.station = st.id;
        if (st.wefax && st.wefax.shift_hz) s.shift_hz = st.wefax.shift_hz;
        const f = (st.wefax && st.wefax.frequencies) || [];
        s.label = (f.length && f[0].callsign ? f[0].callsign : st.id).replace(/[^A-Za-z0-9_.-]/g, '');
      }
      const r = await call('PUT', '/settings', { version: setup.version, settings: s });
      if (r.status === 409) { await setupLoad(); setupMessage('skipper.conflict', null, true); return false; }
      if (r.status !== 200) { setupMessage('skipper.error', { e: r.data.error || r.status }, true); return false; }
      setupMessage(r.data.restart_required ? 'setup.done_restart' : 'setup.done');
      return false;  // stay on the summary with the message
    }
    return true;
  }

  async function startSetup() {
    setup.step = 1;
    setup.station = null;
    setup.input = '';
    setupMessage(null);
    if (await setupLoad()) renderSetupStep();
  }

  // ------------------------------------------------------------------ routing

  function currentView() {
    const h = location.hash.replace(/^#/, '');
    if (h.indexOf('chart/') === 0) return 'chart';
    return ['live', 'charts', 'schedule', 'diag', 'settings', 'setup'].indexOf(h) >= 0 ? h : 'live';
  }

  function route() {
    const view = currentView();
    document.querySelectorAll('.view').forEach(function (v) { v.hidden = v.id !== 'view-' + view; });
    document.querySelectorAll('[data-tab]').forEach(function (a) {
      const tab = a.getAttribute('data-tab');
      a.className = tab === view || (view === 'chart' && tab === 'charts') ? 'active' : '';
    });
    clearInterval(state.scheduleTimer);
    if (view === 'live') { renderLiveInfo(); renderLastChart(); fitLiveHeight(); }
    if (view === 'charts') renderCharts();
    if (view === 'chart') renderChart(decodeURIComponent(location.hash.slice('#chart/'.length)));
    if (view === 'diag') { $('spectrum-wait').hidden = false; renderDiag(); }
    if (view === 'settings') renderSkipper();
    if (view === 'setup') startSetup();
    if (view === 'schedule') {
      renderSchedule();
      state.scheduleTimer = setInterval(renderSchedule, 30000);
    }
  }

  // ------------------------------------------------------------------ start

  function bind() {
    window.addEventListener('hashchange', route);
    window.addEventListener('resize', fitLiveHeight);
    document.querySelectorAll('[data-lang]').forEach(function (b) {
      b.addEventListener('click', function () { setLanguage(b.getAttribute('data-lang')); });
    });
    document.querySelectorAll('[data-theme-choice]').forEach(function (b) {
      b.addEventListener('click', function () { setTheme(b.getAttribute('data-theme-choice')); });
    });
    document.querySelectorAll('[data-sound]').forEach(function (b) {
      b.addEventListener('click', function () {
        setSound(b.getAttribute('data-sound') === 'on');
        if (b.getAttribute('data-sound') === 'on') beep();  // also unlocks audio (user gesture)
      });
    });
    document.querySelectorAll('[data-version]').forEach(function (b) {
      b.addEventListener('click', function () {
        state.chart.version = b.getAttribute('data-version');
        showChartImage();
      });
    });
    $('quality-toggle').addEventListener('click', function () {
      state.chart.light = !state.chart.light;
      showChartImage();
    });
    $('setup-back').addEventListener('click', function () {
      if (setup.step > 1) { setup.step -= 1; setupMessage(null); renderSetupStep(); }
    });
    $('setup-next').addEventListener('click', async function () {
      if (!(await setupLeave())) return;
      if (setup.step < setup.last) { setup.step += 1; renderSetupStep(); }
    });
    $('setup-pw').addEventListener('submit', function (ev) { ev.preventDefault(); $('setup-next').click(); });
    $('form-create').addEventListener('submit', onCreate);
    $('form-login').addEventListener('submit', onLogin);
    $('form-settings').addEventListener('submit', onSave);
    $('form-password').addEventListener('submit', onPassword);
    $('logout').addEventListener('click', onLogout);
    $('restart').addEventListener('click', async function () {
      const r = await call('POST', '/restart');
      if (r.status === 200) message('skipper.restarting');
      else if (r.status === 401) { setToken(''); renderSkipper(); message('skipper.expired', null, true); }
      else message('skipper.error', { e: r.data.error || r.status }, true);
    });
    $('zoom-toggle').addEventListener('click', function () {
      state.chart.fit = !state.chart.fit;
      showChartImage();
    });
  }

  async function about() {
    try {
      const info = await getJSON(API + '/info');
      $('about').textContent = 'openWeatherFaxSSB ' + info.version + ' (' + info.git_revision + ') · ' + info.license;
      // AGPL-3.0 section 13: the exact source of the running program (UI-006).
      $('source-link').href = info.source_url;
    } catch (e) { /* offline */ }
  }

  bind();
  setTheme(load('theme', 'auto'));
  setSound(load('sound', 'off') === 'on');
  const browserLang = (navigator.language || 'fr').toLowerCase().indexOf('fr') === 0 ? 'fr' : 'en';
  setLanguage(load('lang', browserLang));
  about();
  connect();
  // First use: no skipper password yet → open the setup assistant (UI-003).
  call('GET', '/settings').then(function (r) {
    if (r.status === 200 && !r.data.password_set && currentView() === 'live') location.hash = '#setup';
  }).catch(function () { /* offline */ });
}());
