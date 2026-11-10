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

  function renderBadge() {
    const b = $('state');
    const s = state.status;
    const st = s ? s.state : 'offline';
    b.className = 'badge ' + st;
    b.textContent = t('state.' + st) + (st === 'receiving' && s.current ? ' · ' + s.current : '');
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
      let text = t('live.receiving', { label: s.current.split('_').slice(1).join(' '), when: when(stamp), lines: state.live.rows });
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
    if (m.type === 'status') {
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
      if (state.status) { state.status.state = 'receiving'; state.status.current = m.stem; }
      renderBadge();
      renderLiveInfo();
      toast(t('toast.new_start', { label: m.stem }));
    } else if (m.type === 'lock') {
      state.live.locked = t('live.locked', { ppm: signed(m.clock_ppm, 1), hz: signed(m.tuning_offset_hz, 0) }) +
        ', ' + t(m.phasing_found ? 'live.phasing' : 'live.no_phasing');
      renderLiveInfo();
    } else if (m.type === 'line') {
      drawLine(m);
    } else if (m.type === 'complete') {
      if (state.status) { state.status.state = 'listening'; state.status.current = ''; }
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

  // ------------------------------------------------------------------ routing

  function currentView() {
    const h = location.hash.replace(/^#/, '');
    if (h.indexOf('chart/') === 0) return 'chart';
    return ['live', 'charts', 'schedule', 'settings'].indexOf(h) >= 0 ? h : 'live';
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
}());
