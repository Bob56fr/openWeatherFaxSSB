// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Replay player of the web showcase (docs/VITRINE-WEB.md § 7, PLY-001 … PLY-025):
// replays a chart package (meta.json, telemetry.json, raw.webp, enh.webp, thumb.webp,
// audio.m4a) as if it were being received, with its real sound.
//
// The sound is the clock (PLY-001): the line shown is computed from the playback
// position of the <audio> element, so seeking, speeding up or a stalled download can
// never put the image and the sound out of step.
//
// Self-contained so that the same file serves the station dashboard and the WordPress
// plugin: plain ES2017, no framework, no build step, no request before the visitor
// asks for one except meta.json and the thumbnail (PLY-021), no cookie, no tracking
// (SEC-005). Every text that comes from a package is inserted with textContent.
//
//   const p = OwfaxPlayer.create(element, { base: '/api/v1/vitrine/<chart_id>/' });
//   options: lang ('fr' | 'en'), auto (playlist: start when visible, pause when the
//   tab is hidden), sound (try to start with the sound on: the visitor already chose
//   it on the previous chart), rate (1, 4 or 10), onEnded (callback), onError (callback).

'use strict';

(function (root) {
  const SVGNS = 'http://www.w3.org/2000/svg';
  const STRIP_GAP = 8;     // px of the chart scale between the image and the quality strip
  const STRIP_WIDTH = 40;  // quality strip (PLY-012), in px of the chart scale
  const RATES = [1, 4, 10];

  const STRINGS = {
    fr: {
      play: 'Lecture', pause: 'Pause', listen: 'Écouter', mute: 'Couper le son',
      speed: 'Vitesse de lecture', raw: 'Avant (brute)', enhanced: 'Après (améliorée)',
      before_after: 'Image brute ou améliorée',
      'phase.wait': 'Écoute de la fréquence', 'phase.apt': 'Tonalité de démarrage (APT)',
      'phase.phasing': 'Phasage', 'phase.image': "Réception de l'image",
      'phase.stop': "Tonalité d'arrêt", 'phase.end': 'Fin de la carte',
      replay: 'Rediffusion de la réception du {date}',
      smeter: 'S-mètre', vu: 'Niveau audio',
      snr: 'SNR', quality: 'Qualité de la ligne', level: 'Niveau audio', line: 'Ligne',
      time: 'Temps',
      seek: 'Position de lecture', seek_text: '{time} sur {total}, ligne {line} sur {lines}',
      chart: 'Carte météo de {station}, révélée ligne par ligne',
      strip: 'À droite de la carte, la qualité de chaque ligne : vert bonne, rouge perdue.',
      zones: 'Hachures : perturbation locale, son coupé ({causes}).',
      'cause.overload': 'saturation du récepteur',
      'cause.impulsive_interference': 'parasite impulsif du bord',
      'cause.broadband_interference': 'parasite large bande du bord',
      distance: '{km} km ({nm} NM), azimut {az}°',
      'daylight.day': 'trajet de jour', 'daylight.night': 'trajet de nuit',
      'daylight.mixed': 'trajet jour et nuit',
      power: '{kw} kW', receiver: 'Récepteur : {rig}',
      diversity_yes: 'diversité : {n} récepteurs', diversity_no: 'sans diversité',
      treatments: "Traitements de l'image améliorée : {list}", none: 'aucun',
      'treatment.clicks': 'suppression des clics', 'treatment.denoise': 'débruitage',
      'treatment.deskew': "redressement de l'inclinaison", 'treatment.contrast': 'contraste',
      loading: 'Chargement…', error: 'Impossible de charger cette carte.',
      terms: "Conditions d'utilisation", rates_mute: 'Au-delà de ×4, certains navigateurs coupent le son.',
    },
    en: {
      play: 'Play', pause: 'Pause', listen: 'Listen', mute: 'Mute',
      speed: 'Playback speed', raw: 'Before (raw)', enhanced: 'After (enhanced)',
      before_after: 'Raw or enhanced image',
      'phase.wait': 'Listening on the frequency', 'phase.apt': 'Start tone (APT)',
      'phase.phasing': 'Phasing', 'phase.image': 'Receiving the image',
      'phase.stop': 'Stop tone', 'phase.end': 'End of the chart',
      replay: 'Replay of the reception of {date}',
      smeter: 'S-meter', vu: 'Audio level',
      snr: 'SNR', quality: 'Line quality', level: 'Audio level', line: 'Line', time: 'Time',
      seek: 'Playback position', seek_text: '{time} of {total}, line {line} of {lines}',
      chart: 'Weather chart from {station}, revealed line by line',
      strip: 'Right of the chart, the quality of each line: green good, red lost.',
      zones: 'Hatched: local disturbance, sound muted ({causes}).',
      'cause.overload': 'receiver overload',
      'cause.impulsive_interference': 'impulsive interference on board',
      'cause.broadband_interference': 'broadband interference on board',
      distance: '{km} km ({nm} NM), bearing {az}°',
      'daylight.day': 'daylight path', 'daylight.night': 'night path',
      'daylight.mixed': 'day and night path',
      power: '{kw} kW', receiver: 'Receiver: {rig}',
      diversity_yes: 'diversity: {n} receivers', diversity_no: 'no diversity',
      treatments: 'Enhanced image treatments: {list}', none: 'none',
      'treatment.clicks': 'click removal', 'treatment.denoise': 'denoising',
      'treatment.deskew': 'deskew', 'treatment.contrast': 'contrast',
      loading: 'Loading…', error: 'This chart could not be loaded.',
      terms: 'Terms of use', rates_mute: 'Above ×4, some browsers mute the sound.',
    },
  };

  // ------------------------------------------------------------ pure helpers

  function clamp(x, a, b) { return x < a ? a : x > b ? b : x; }

  /** Line period: image duration over lines, so that the chart is complete exactly at
   *  image_end_s (line_period_s is rounded in meta.json). */
  function linePeriod(sync, lines) {
    return sync.image_end_s > sync.image_s && lines > 0
      ? (sync.image_end_s - sync.image_s) / lines : sync.line_period_s;
  }

  /** Lines fully revealed at time `t` of the sound, and how much of the next one. */
  function lineAt(sync, lines, t) {
    if (!(t >= sync.image_s)) return { done: 0, frac: 0 };
    const x = (t - sync.image_s) / linePeriod(sync, lines);
    if (x >= lines) return { done: lines, frac: 0 };
    const done = Math.floor(x);
    return { done: done, frac: x - done };
  }

  /** Phase of the transmission at time `t` (PLY-002). */
  function phaseAt(sync, t, duration) {
    if (duration > 0 && t >= duration - 0.05) return 'end';
    if (t >= sync.image_end_s) return 'stop';
    if (t >= sync.image_s) return 'image';
    if (sync.phasing_s !== null && sync.phasing_s !== undefined && t >= sync.phasing_s) {
      return 'phasing';
    }
    if (sync.apt_start_s !== null && sync.apt_start_s !== undefined && t >= sync.apt_start_s) {
      return 'apt';
    }
    return 'wait';
  }

  /** Value of a per-second series at time `t`, value i standing for [i, i+1). */
  function sampleAt(series, t, interval) {
    if (!series || series.length === 0) return null;
    const x = t / (interval || 1) - 0.5;
    if (x <= 0) return series[0];
    const i = Math.floor(x);
    if (i >= series.length - 1) return series[series.length - 1];
    const a = series[i], b = series[i + 1];
    if (a === null || b === null) return a === null ? b : a;
    return a + (b - a) * (x - i);
  }

  /** S-meter needle position 0 … 1: S0 … S9 on the first 60 %, S9 … S9+60 after. */
  function meterPosition(dbS9) {
    if (dbS9 <= 0) return clamp((dbS9 + 54) / 54, 0, 1) * 0.6;
    return 0.6 + clamp(dbS9 / 60, 0, 1) * 0.4;
  }

  /** Audio level needle position 0 … 1, −60 … 0 dBFS. */
  function vuPosition(dbfs) { return clamp((dbfs + 60) / 60, 0, 1); }

  /** "S7", "S9", "S9+20" (S units of 6 dB, Hamlib STRENGTH in dB relative to S9). */
  function sLabel(dbS9) {
    if (dbS9 > 0.5) return 'S9+' + Math.round(dbS9);
    return 'S' + clamp(Math.round(9 + dbS9 / 6), 0, 9);
  }

  /** Colour of a line quality 0 … 1: red (lost) … green (good). */
  function qualityColor(q) {
    const hue = Math.round(clamp((q - 0.5) / 0.45, 0, 1) * 120);
    return 'hsl(' + hue + ', 75%, 42%)';
  }

  function formatTime(s) {
    const v = Math.max(0, Math.floor(s || 0));
    return Math.floor(v / 60) + ':' + String(v % 60).padStart(2, '0');
  }

  /** "2026-11-09T21:45:58Z" → "2026-11-09 21:45 UTC" (always UTC, as at sea). */
  function formatUtc(iso) {
    const m = /^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2})/.exec(iso || '');
    return m ? m[1] + ' ' + m[2] + ' UTC' : (iso || '');
  }

  /** Zones of local disturbance (hatched, muted), from meta.json. */
  function localZones(meta) {
    const d = meta && meta.disturbances;
    if (!d || !Array.isArray(d.zones)) return [];
    return d.zones.filter(function (z) { return z.local; });
  }

  // ------------------------------------------------------------------ player

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }

  function svg(tag, attrs) {
    const e = document.createElementNS(SVGNS, tag);
    Object.keys(attrs || {}).forEach(function (k) { e.setAttribute(k, attrs[k]); });
    return e;
  }

  async function getJSON(url) {
    const r = await fetch(url, { credentials: 'same-origin' });
    if (!r.ok) throw new Error(url + ': HTTP ' + r.status);
    return r.json();
  }

  function loadImage(url) {
    return new Promise(function (resolve, reject) {
      const img = new Image();
      img.decoding = 'async';
      img.onload = function () { resolve(img); };
      img.onerror = function () { reject(new Error(url + ': image')); };
      img.src = url;
    });
  }

  function Player(container, options) {
    this.root = container;
    this.opt = Object.assign({ lang: 'fr', auto: false, sound: false, rate: 1 }, options || {});
    this.base = this.opt.base.replace(/\/?$/, '/');
    this.s = STRINGS[this.opt.lang] || STRINGS.fr;
    this.meta = null;
    this.tele = null;
    this.images = {};      // raw, enhanced
    this.view = 'raw';     // image being shown (PLY-005)
    this.drawn = 0;        // lines fully drawn on the canvas
    this.started = false;
    this.needle = 0;       // displayed needle position (ballistics)
    this.lastFrame = 0;
    this.lastText = 0;
    this.follow = 0;       // until then, the visitor scrolls the screen: no following
    this.frame = null;
    this.autoPaused = false;
    this.destroyed = false;
  }

  Player.prototype.t = function (key, vars) {
    let s = Object.prototype.hasOwnProperty.call(this.s, key) ? this.s[key] : key;
    Object.keys(vars || {}).forEach(function (k) { s = s.split('{' + k + '}').join(String(vars[k])); });
    return s;
  };

  Player.prototype.num = function (x, digits) {
    return new Intl.NumberFormat(this.opt.lang, {
      minimumFractionDigits: digits || 0, maximumFractionDigits: digits || 0,
    }).format(x);
  };

  /** Loads meta.json and shows the thumbnail: nothing else before the visitor acts. */
  Player.prototype.load = async function () {
    this.root.textContent = '';
    this.root.classList.remove('owp-started', 'owp-playing');
    this.root.classList.add('owp');
    const wait = el('p', 'owp-message', this.t('loading'));
    this.root.appendChild(wait);
    try {
      this.meta = await getJSON(this.base + 'meta.json');
    } catch (e) {
      wait.textContent = this.t('error');
      if (this.opt.onError) this.opt.onError(e);
      return this;
    }
    this.root.removeChild(wait);
    this.build();
    if (this.opt.auto) this.watchVisibility();
    return this;
  };

  Player.prototype.file = function (key, fallback) {
    const f = this.meta.files || {};
    return this.base + encodeURIComponent(f[key] || fallback);
  };

  Player.prototype.build = function () {
    const m = this.meta, self = this;
    const st = m.station || {};
    this.lines = (m.wefax && m.wefax.lines) || 0;
    this.width = (m.wefax && m.wefax.width_px) || 1810;
    this.sync = m.sync;
    this.duration = (m.sync && m.sync.audio_duration_s) || 0;
    this.zones = localZones(m);

    // Information banner (PLY-015).
    const banner = el('div', 'owp-banner');
    const title = el('h2', 'owp-title');
    title.appendChild(el('span', 'owp-station', st.name || st.id || ''));
    const freq = [st.callsign, st.assigned_khz ? this.num(st.assigned_khz, st.assigned_khz % 1 ? 1 : 0) + ' kHz' : '']
      .filter(Boolean).join(' · ');
    title.appendChild(el('span', 'owp-freq', freq));
    banner.appendChild(title);
    banner.appendChild(el('p', 'owp-replay',
      this.t('replay', { date: formatUtc(m.time && m.time.start_utc) })));
    const facts = [];
    const p = m.path || {};
    if (p.distance_km !== undefined) {
      facts.push(this.t('distance', { km: this.num(p.distance_km), nm: this.num(p.distance_nm),
        az: this.num(p.azimuth_deg) }));
    }
    if (p.daylight) facts.push(this.t('daylight.' + p.daylight));
    if (st.power_kw) facts.push(this.t('power', { kw: this.num(st.power_kw, st.power_kw % 1 ? 1 : 0) }));
    const div = m.diversity || {};
    facts.push(div.enabled ? this.t('diversity_yes', { n: (div.sources || []).length })
      : this.t('diversity_no'));
    const ul = el('ul', 'owp-facts');
    facts.forEach(function (f) { ul.appendChild(el('li', null, f)); });
    banner.appendChild(ul);
    const rx = m.receiver || {};
    const extra = [];
    if (rx.rig) extra.push(this.t('receiver', { rig: rx.rig }));
    const tr = (m.treatments && m.treatments.enhanced) || [];
    extra.push(this.t('treatments', { list: tr.length
      ? tr.map(function (x) { return self.s['treatment.' + x] || x; }).join(', ')
      : this.t('none') }));
    banner.appendChild(el('p', 'owp-receiver', extra.join(' — ')));
    this.root.appendChild(banner);

    // Screen: the chart canvas in a scrolling viewport, the phase above it.
    const body = el('div', 'owp-body');
    const screen = el('div', 'owp-screen');
    this.phaseEl = el('p', 'owp-phase', '');
    this.phaseEl.setAttribute('aria-live', 'polite');
    screen.appendChild(this.phaseEl);
    this.viewport = el('div', 'owp-viewport');
    this.canvas = el('canvas', 'owp-canvas');
    this.canvas.width = this.width + STRIP_GAP + STRIP_WIDTH;
    this.canvas.height = Math.max(1, this.lines);
    this.canvas.setAttribute('role', 'img');
    this.canvas.setAttribute('aria-label', this.t('chart', { station: st.name || st.id || '' }));
    this.ctx = this.canvas.getContext('2d');
    this.poster = el('img', 'owp-poster');
    this.poster.alt = '';
    this.poster.src = this.file('thumb', 'thumb.webp');
    this.bigPlay = el('button', 'owp-bigplay');
    this.bigPlay.type = 'button';
    this.bigPlay.setAttribute('aria-label', this.t('play'));
    this.bigPlay.appendChild(this.icon('play'));
    this.viewport.appendChild(this.poster);
    this.viewport.appendChild(this.canvas);
    this.viewport.appendChild(this.bigPlay);
    screen.appendChild(this.viewport);
    const legend = el('p', 'owp-legend', this.t('strip'));
    if (this.zones.length) {
      const causes = [];
      this.zones.forEach(function (z) {
        const c = self.t('cause.' + z.cause);
        if (causes.indexOf(c) < 0) causes.push(c);
      });
      legend.appendChild(document.createTextNode(' '));
      legend.appendChild(el('span', 'owp-hatch-key', this.t('zones', { causes: causes.join(', ') })));
    }
    screen.appendChild(legend);
    body.appendChild(screen);

    // Instruments (PLY-010, PLY-011).
    const inst = el('div', 'owp-instruments');
    inst.appendChild(this.buildMeter());
    const dl = el('dl', 'owp-values');
    this.values = {};
    [['snr', 'snr'], ['quality', 'quality'], ['level', 'level'], ['line', 'line'], ['time', 'time']]
      .forEach(function (k) {
        const d = el('div');
        d.appendChild(el('dt', null, self.t(k[1])));
        const dd = el('dd', null, '—');
        d.appendChild(dd);
        dl.appendChild(d);
        self.values[k[0]] = dd;
      });
    inst.appendChild(dl);
    body.appendChild(inst);
    this.root.appendChild(body);

    // Timeline: SNR, quality and disturbances over the whole chart; seek bar (PLY-014).
    this.timeline = el('canvas', 'owp-timeline');
    this.timeline.tabIndex = 0;
    this.timeline.setAttribute('role', 'slider');
    this.timeline.setAttribute('aria-label', this.t('seek'));
    this.timeline.setAttribute('aria-valuemin', '0');
    this.timeline.setAttribute('aria-valuemax', String(Math.round(this.duration)));
    this.root.appendChild(this.timeline);

    // Controls.
    const ctl = el('div', 'owp-controls');
    this.playBtn = this.button('owp-play', 'play', this.t('play'));
    this.listenBtn = this.button('owp-listen', 'sound', this.t('listen'));
    this.listenBtn.setAttribute('aria-pressed', 'false');
    ctl.appendChild(this.playBtn);
    ctl.appendChild(this.listenBtn);
    const rates = el('div', 'owp-group');
    rates.setAttribute('role', 'group');
    rates.setAttribute('aria-label', this.t('speed'));
    rates.title = this.t('rates_mute');
    this.rateBtns = RATES.map(function (r) {
      const b = el('button', 'owp-rate', '×' + r);
      b.type = 'button';
      b.setAttribute('aria-pressed', String(r === self.opt.rate));
      b.addEventListener('click', function () { self.setRate(r); });
      rates.appendChild(b);
      return b;
    });
    ctl.appendChild(rates);
    this.versions = el('div', 'owp-group');
    this.versions.setAttribute('role', 'group');
    this.versions.setAttribute('aria-label', this.t('before_after'));
    this.versionBtns = ['raw', 'enhanced'].map(function (v) {
      const b = el('button', 'owp-version', self.t(v));
      b.type = 'button';
      b.setAttribute('aria-pressed', String(v === 'raw'));
      b.addEventListener('click', function () { self.showVersion(v); });
      self.versions.appendChild(b);
      return b;
    });
    this.versions.hidden = true;
    ctl.appendChild(this.versions);
    this.root.appendChild(ctl);

    // Attribution of the weather service (PUB-024), always visible.
    const attr = el('p', 'owp-attribution', st.attribution || '');
    if (st.terms_url && /^https?:\/\//.test(st.terms_url)) {
      attr.appendChild(document.createTextNode(' — '));
      const a = el('a', null, this.t('terms'));
      a.href = st.terms_url;
      a.rel = 'noopener';
      a.target = '_blank';
      attr.appendChild(a);
    }
    this.root.appendChild(attr);

    // The sound: muted until the visitor asks (PLY-004), nothing loaded before (PLY-021).
    this.audio = el('audio');
    this.audio.preload = 'none';
    this.audio.muted = !this.opt.sound;
    this.audio.setAttribute('playsinline', '');
    this.root.appendChild(this.audio);

    this.wire();
    this.renderSound();
    this.drawTimeline();
    this.renderValues(0);
  };

  Player.prototype.icon = function (name) {
    const s = svg('svg', { viewBox: '0 0 24 24', 'aria-hidden': 'true', focusable: 'false' });
    const paths = {
      play: 'M7 4.5v15l12.5-7.5z',
      pause: 'M6 4.5h4.5v15H6zM13.5 4.5H18v15h-4.5z',
      sound: 'M4 9.5h3.5L12 5v14l-4.5-4.5H4zM15 8.5a5 5 0 0 1 0 7M17.5 6a8.5 8.5 0 0 1 0 12',
      muted: 'M4 9.5h3.5L12 5v14l-4.5-4.5H4zM15.5 9.5l5 5M20.5 9.5l-5 5',
    };
    const filled = name === 'play' || name === 'pause';
    s.appendChild(svg('path', { d: paths[name], fill: filled ? 'currentColor' : 'none',
      stroke: filled ? 'none' : 'currentColor', 'stroke-width': '1.8',
      'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    if (!filled) s.firstChild.setAttribute('fill', 'none');
    return s;
  };

  Player.prototype.button = function (cls, icon, label) {
    const b = el('button', cls);
    b.type = 'button';
    b.appendChild(this.icon(icon));
    b.appendChild(el('span', null, label));
    return b;
  };

  Player.prototype.setButton = function (b, icon, label) {
    b.replaceChild(this.icon(icon), b.firstChild);
    b.lastChild.textContent = label;
  };

  /** S-meter, or audio level meter when the capture has no S-meter readings (PLY-010). */
  Player.prototype.buildMeter = function () {
    const box = el('div', 'owp-meter');
    const s = svg('svg', { viewBox: '0 0 200 118', role: 'img' });
    const cx = 100, cy = 108, r = 84;
    const pt = function (pos, rad) {
      const a = (-50 + 100 * pos) * Math.PI / 180;
      return [cx + rad * Math.sin(a), cy - rad * Math.cos(a)];
    };
    const arc = function (p0, p1, rad, cls) {
      const a = pt(p0, rad), b = pt(p1, rad);
      s.appendChild(svg('path', { d: 'M' + a[0] + ' ' + a[1] + ' A' + rad + ' ' + rad + ' 0 0 1 ' +
        b[0] + ' ' + b[1], class: cls, fill: 'none' }));
    };
    const ticks = [];
    // S-meter scale first; chosen again once telemetry.json says whether there are
    // S-meter readings (an SDR has none: audio level meter).
    this.meterSvg = s;
    this.meterBox = box;
    this.meterCaption = el('p', 'owp-meter-caption', this.t('smeter'));
    this.meterReading = el('p', 'owp-meter-reading', '—');
    this.drawScale = function (smeter) {
      while (s.firstChild) s.removeChild(s.firstChild);
      ticks.length = 0;
      if (smeter) {
        [-48, -36, -24, -12, 0].forEach(function (db, i) {
          ticks.push([meterPosition(db), String(1 + 2 * i), false]);
        });
        [20, 40, 60].forEach(function (db) { ticks.push([meterPosition(db), '+' + db, true]); });
        arc(0, 0.6, r, 'owp-scale');
        arc(0.6, 1, r, 'owp-scale owp-hot');
      } else {
        [-60, -40, -20, -10, 0].forEach(function (db) {
          ticks.push([vuPosition(db), String(db), db > -10]);
        });
        arc(0, vuPosition(-10), r, 'owp-scale');
        arc(vuPosition(-10), 1, r, 'owp-scale owp-hot');
      }
      ticks.forEach(function (tk) {
        const a = pt(tk[0], r - 2), b = pt(tk[0], r + 7), l = pt(tk[0], r + 17);
        s.appendChild(svg('line', { x1: a[0], y1: a[1], x2: b[0], y2: b[1],
          class: tk[2] ? 'owp-tick owp-hot' : 'owp-tick' }));
        const tx = svg('text', { x: l[0], y: l[1] + 3, class: tk[2] ? 'owp-label owp-hot' : 'owp-label',
          'text-anchor': 'middle' });
        tx.textContent = tk[1];
        s.appendChild(tx);
      });
      const unit = svg('text', { x: 100, y: 70, class: 'owp-unit', 'text-anchor': 'middle' });
      unit.textContent = smeter ? 'S' : 'dBFS';
      s.appendChild(unit);
      this.needleEl = svg('line', { x1: cx, y1: cy, x2: cx, y2: cy - r + 4, class: 'owp-needle' });
      s.appendChild(this.needleEl);
      s.appendChild(svg('circle', { cx: cx, cy: cy, r: 5, class: 'owp-hub' }));
      this.meterCaption.textContent = this.t(smeter ? 'smeter' : 'vu');
      s.setAttribute('aria-label', this.t(smeter ? 'smeter' : 'vu'));
      this.setNeedle(this.needle);
    };
    box.appendChild(this.meterCaption);
    box.appendChild(s);
    box.appendChild(this.meterReading);
    this.drawScale(true);
    return box;
  };

  Player.prototype.setNeedle = function (pos) {
    if (this.needleEl) {
      this.needleEl.setAttribute('transform', 'rotate(' + (-50 + 100 * pos).toFixed(2) + ' 100 108)');
    }
  };

  Player.prototype.wire = function () {
    const self = this, a = this.audio;
    this.playBtn.addEventListener('click', function () { self.toggle(); });
    this.bigPlay.addEventListener('click', function () { self.play(); });
    this.listenBtn.addEventListener('click', function () {
      a.muted = !a.muted;
      self.autoPaused = false;
      self.renderSound();
      if (!a.muted && a.paused) self.play();
    });
    ['play', 'pause', 'ended'].forEach(function (ev) {
      a.addEventListener(ev, function () {
        self.renderButtons();
        if (ev === 'play') self.loop();
        if (ev === 'ended') {
          self.render(true);
          if (self.opt.onEnded) self.opt.onEnded(self);
        }
      });
    });
    ['seeked', 'timeupdate', 'loadedmetadata'].forEach(function (ev) {
      a.addEventListener(ev, function () { if (a.paused) self.render(true); });
    });
    a.addEventListener('loadedmetadata', function () {
      if (self.pendingSeek !== undefined) {
        a.currentTime = self.pendingSeek;
        self.pendingSeek = undefined;
      }
      self.applyRate();
    });
    // The user scrolls the screen: stop following the current line for a while.
    ['wheel', 'touchmove'].forEach(function (ev) {
      self.viewport.addEventListener(ev, function () { self.follow = Date.now() + 4000; },
        { passive: true });
    });

    // Seek bar: pointer and keyboard (PLY-024).
    const seekTo = function (e) {
      const r = self.timeline.getBoundingClientRect();
      self.seek(clamp((e.clientX - r.left) / r.width, 0, 1) * self.duration);
    };
    let dragging = false;
    this.timeline.addEventListener('pointerdown', function (e) {
      dragging = true;
      if (self.timeline.setPointerCapture) self.timeline.setPointerCapture(e.pointerId);
      seekTo(e);
    });
    this.timeline.addEventListener('pointermove', function (e) { if (dragging) seekTo(e); });
    ['pointerup', 'pointercancel'].forEach(function (ev) {
      self.timeline.addEventListener(ev, function () { dragging = false; });
    });
    this.timeline.addEventListener('keydown', function (e) {
      const now = self.time();
      const step = { ArrowLeft: -5, ArrowRight: 5, ArrowDown: -5, ArrowUp: 5, PageDown: -60, PageUp: 60 };
      if (Object.prototype.hasOwnProperty.call(step, e.key)) self.seek(now + step[e.key]);
      else if (e.key === 'Home') self.seek(0);
      else if (e.key === 'End') self.seek(self.duration);
      else if (e.key === ' ' || e.key === 'Enter') self.toggle();
      else return;
      e.preventDefault();
    });
    if (typeof ResizeObserver === 'function') {
      this.resizer = new ResizeObserver(function () { self.drawTimeline(); self.render(true); });
      this.resizer.observe(this.timeline);
    } else {
      window.addEventListener('resize', function () { self.drawTimeline(); self.render(true); });
    }
  };

  /** Pauses when the tab is hidden, and plays when visible, in playlist mode (PLY-021/022). */
  Player.prototype.watchVisibility = function () {
    const self = this;
    this.onVisibility = function () {
      if (document.hidden && !self.audio.paused) {
        self.audio.pause();
        self.autoPaused = true;
      } else if (!document.hidden && self.autoPaused && self.visible) {
        self.autoPaused = false;
        self.play();
      }
    };
    document.addEventListener('visibilitychange', this.onVisibility);
    if (typeof IntersectionObserver === 'function') {
      this.observer = new IntersectionObserver(function (entries) {
        self.visible = entries[0].isIntersecting;
        if (self.visible && !document.hidden && (!self.started || self.autoPaused)) {
          self.autoPaused = false;
          self.play();
        } else if (!self.visible && !self.audio.paused) {
          self.audio.pause();
          self.autoPaused = true;
        }
      }, { threshold: 0.4 });
      this.observer.observe(this.root);
    } else {
      this.visible = true;
      this.play();
    }
  };

  /** Downloads the image, the telemetry and the sound, on the first play only. */
  Player.prototype.start = async function () {
    if (this.starting) return this.starting;
    const self = this;
    this.starting = (async function () {
      self.phaseEl.textContent = self.t('loading');
      const results = await Promise.all([
        getJSON(self.file('telemetry', 'telemetry.json')),
        loadImage(self.file('raw', 'raw.webp')),
      ]);
      self.tele = results[0];
      self.images.raw = results[1];
      self.drawScale(Array.isArray(self.tele.smeter_db_s9) && self.tele.smeter_db_s9.length > 0);
      self.audio.src = self.file('audio', 'audio.m4a');
      self.audio.preload = 'auto';
      self.started = true;
      self.poster.hidden = true;
      self.bigPlay.hidden = true;
      self.root.classList.add('owp-started');
      self.drawTimeline();
    })();
    try {
      await this.starting;
    } catch (e) {
      this.starting = null;
      this.phaseEl.textContent = this.t('error');
      if (this.opt.onError) this.opt.onError(e);
      throw e;
    }
    return this.starting;
  };

  Player.prototype.play = async function () {
    try {
      await this.start();
    } catch (e) {
      return;
    }
    this.applyRate();
    if (this.audio.ended || this.time() >= this.duration - 0.05) this.seek(0);
    try {
      await this.audio.play();
    } catch (e) {
      // Autoplay refused (sound on without a gesture): retry muted, sound stays a choice.
      if (!this.audio.muted) {
        this.audio.muted = true;
        this.renderSound();
        try { await this.audio.play(); } catch (e2) { /* the visitor will press Play */ }
      }
    }
  };

  Player.prototype.toggle = function () {
    this.autoPaused = false;
    if (!this.started || this.audio.paused) this.play();
    else this.audio.pause();
  };

  Player.prototype.time = function () {
    if (!this.started) return this.pendingSeek || 0;
    return this.audio.readyState > 0 ? this.audio.currentTime : (this.pendingSeek || 0);
  };

  Player.prototype.seek = function (t) {
    const v = clamp(t, 0, this.duration);
    if (this.started && this.audio.readyState > 0) this.audio.currentTime = v;
    else this.pendingSeek = v;
    this.follow = 0;
    this.render(true);
  };

  Player.prototype.setRate = function (r) {
    this.opt.rate = r;
    this.rateBtns.forEach(function (b, i) { b.setAttribute('aria-pressed', String(RATES[i] === r)); });
    this.applyRate();
  };

  Player.prototype.applyRate = function () {
    const a = this.audio;
    // The pitch is kept when speeding up (PLY-003).
    a.preservesPitch = true;
    a.mozPreservesPitch = true;
    a.webkitPreservesPitch = true;
    a.defaultPlaybackRate = this.opt.rate;
    a.playbackRate = this.opt.rate;
  };

  Player.prototype.renderButtons = function () {
    const playing = this.started && !this.audio.paused;
    this.setButton(this.playBtn, playing ? 'pause' : 'play', this.t(playing ? 'pause' : 'play'));
    this.root.classList.toggle('owp-playing', playing);
  };

  Player.prototype.renderSound = function () {
    const on = !this.audio.muted;
    this.setButton(this.listenBtn, on ? 'muted' : 'sound', this.t(on ? 'mute' : 'listen'));
    this.listenBtn.setAttribute('aria-pressed', String(on));
  };

  Player.prototype.showVersion = async function (v) {
    if (v === 'enhanced' && !this.images.enhanced) {
      try {
        this.images.enhanced = await loadImage(this.file('enhanced', 'enh.webp'));
      } catch (e) {
        return;
      }
    }
    this.view = v;
    const self = this;
    this.versionBtns.forEach(function (b, i) {
      b.setAttribute('aria-pressed', String(['raw', 'enhanced'][i] === self.view));
    });
    this.drawn = 0;
    this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    this.render(true);
  };

  Player.prototype.loop = function () {
    if (this.frame !== null || this.destroyed) return;
    const self = this;
    const step = function () {
      self.frame = null;
      self.render(false);
      if (self.started && !self.audio.paused && !self.destroyed) {
        self.frame = window.requestAnimationFrame(step);
      }
    };
    this.frame = window.requestAnimationFrame(step);
  };

  /** Draws everything for the current sound position. */
  Player.prototype.render = function (force) {
    if (!this.meta) return;
    const t = this.time();
    const now = (typeof performance !== 'undefined' ? performance : Date).now();
    const dt = this.lastFrame ? Math.min(0.5, (now - this.lastFrame) / 1000) : 0;
    this.lastFrame = now;
    if (this.started) this.drawChart(t);
    this.renderMeter(t, force ? 0 : dt);
    if (force || now - this.lastText > 100) {
      this.lastText = now;
      this.renderValues(t);
    }
    this.drawCursor(t);
  };

  Player.prototype.drawChart = function (t) {
    const img = this.images[this.view] || this.images.raw;
    if (!img) return;
    const lines = this.lines, w = this.width, ctx = this.ctx;
    const pos = lineAt(this.sync, lines, t);
    const finished = pos.done >= lines;
    // Showing the enhanced version only makes sense on a complete chart.
    if (!finished && this.view !== 'raw') {
      this.view = 'raw';
      this.versionBtns[0].setAttribute('aria-pressed', 'true');
      this.versionBtns[1].setAttribute('aria-pressed', 'false');
      this.drawn = 0;
      ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
      return this.drawChart(t);
    }
    this.versions.hidden = !finished;
    const sy = img.naturalHeight / lines;  // in case the image is not one row per line
    if (pos.done < this.drawn) {
      ctx.clearRect(0, pos.done, this.canvas.width, this.drawn + 1 - pos.done);
      this.drawn = pos.done;
    }
    if (pos.done > this.drawn) {
      const a = this.drawn, n = pos.done - a;
      ctx.clearRect(0, a, w, n + 1);
      ctx.drawImage(img, 0, a * sy, img.naturalWidth, n * sy, 0, a, w, n);
      this.drawStrip(a, pos.done);
      this.drawHatches(a, pos.done);
      this.drawn = pos.done;
    }
    // The line being received appears from left to right, as on the receiver.
    if (!finished) {
      ctx.clearRect(0, pos.done, w, 1);
      const pw = Math.max(1, Math.round(w * pos.frac));
      if (pos.frac > 0) {
        ctx.drawImage(img, 0, pos.done * sy, img.naturalWidth * pw / w, sy, 0, pos.done, pw, 1);
      }
    }
    this.followLine(pos.done);
  };

  Player.prototype.drawStrip = function (a, b) {
    const q = (this.tele && this.tele.line_quality) || [];
    const x = this.width + STRIP_GAP;
    for (let i = a; i < b; ++i) {
      this.ctx.fillStyle = qualityColor(q[i] === undefined ? 1 : q[i]);
      this.ctx.fillRect(x, i, STRIP_WIDTH, 1);
    }
  };

  Player.prototype.hatchPattern = function () {
    if (this.pattern) return this.pattern;
    const c = document.createElement('canvas');
    c.width = c.height = 24;
    const g = c.getContext('2d');
    g.strokeStyle = 'rgba(230, 120, 0, 0.6)';
    g.lineWidth = 4;
    g.beginPath();
    for (let k = -24; k <= 48; k += 12) {
      g.moveTo(k, 24);
      g.lineTo(k + 24, 0);
    }
    g.stroke();
    this.pattern = this.ctx.createPattern(c, 'repeat');
    return this.pattern;
  };

  Player.prototype.drawHatches = function (a, b) {
    const self = this;
    this.zones.forEach(function (z) {
      const from = Math.max(a, z.first_line), to = Math.min(b, z.last_line + 1);
      if (to <= from) return;
      self.ctx.fillStyle = self.hatchPattern();
      self.ctx.fillRect(0, from, self.width, to - from);
    });
  };

  /** Keeps the line being received in view, unless the visitor is scrolling. */
  Player.prototype.followLine = function (line) {
    if (Date.now() < this.follow) return;
    const vp = this.viewport;
    const scale = this.canvas.clientHeight / Math.max(1, this.lines);
    const y = line * scale;
    const target = Math.max(0, y - vp.clientHeight * 0.75);
    if (Math.abs(vp.scrollTop - target) > 1) vp.scrollTop = target;
  };

  Player.prototype.renderMeter = function (t, dt) {
    let pos = 0, text = '—';
    const tele = this.tele;
    if (tele) {
      const level = sampleAt(tele.audio_dbfs, t, tele.interval_s);
      const sm = sampleAt(tele.smeter_db_s9, t, tele.interval_s);
      const playing = this.started && !this.audio.paused;
      if (sm !== null) {
        // A visual effect (PUB-052): readings every 10 s, livened up by the audio level.
        const med = this.medianLevel === undefined ? (this.medianLevel = median(tele.audio_dbfs)) : this.medianLevel;
        const live = sm + (level !== null ? (level - med) * 0.6 : 0) + (playing ? (Math.random() - 0.5) * 1.5 : 0);
        pos = meterPosition(live);
        text = sLabel(sm) + ' (' + (sm >= 0 ? '+' : '') + this.num(sm) + ' dB)';
      } else if (level !== null) {
        pos = vuPosition(level + (playing ? (Math.random() - 0.5) * 1.2 : 0));
        text = this.num(level, 1) + ' dBFS';
      }
    }
    // Ballistics: quick rise, slower fall, like a real needle.
    if (dt > 0) {
      const tau = pos > this.needle ? 0.12 : 0.45;
      this.needle += (pos - this.needle) * (1 - Math.exp(-dt / tau));
    } else {
      this.needle = pos;
    }
    this.setNeedle(this.needle);
    this.meterReading.textContent = text;
  };

  Player.prototype.renderValues = function (t) {
    const tele = this.tele, v = this.values;
    const pos = lineAt(this.sync, this.lines, t);
    const phase = phaseAt(this.sync, t, this.duration);
    const label = this.t('phase.' + phase);
    if (this.started && this.phaseEl.textContent !== label) this.phaseEl.textContent = label;
    this.root.setAttribute('data-phase', phase);
    if (tele) {
      const snr = sampleAt(tele.snr_db, t, tele.interval_s);
      const level = sampleAt(tele.audio_dbfs, t, tele.interval_s);
      v.snr.textContent = snr === null ? '—' : this.num(snr, 1) + ' dB';
      v.level.textContent = level === null ? '—' : this.num(level, 1) + ' dBFS';
      const q = tele.line_quality || [];
      const li = Math.min(pos.done, this.lines - 1);
      v.quality.textContent = phase === 'image' && q[li] !== undefined
        ? this.num(Math.round(q[li] * 100)) + ' %' : '—';
    }
    v.line.textContent = this.num(Math.min(this.lines, pos.done + (phase === 'image' ? 1 : 0))) +
      ' / ' + this.num(this.lines);
    v.time.textContent = formatTime(t) + ' / ' + formatTime(this.duration);
    this.timeline.setAttribute('aria-valuenow', String(Math.round(t)));
    this.timeline.setAttribute('aria-valuetext', this.t('seek_text', { time: formatTime(t),
      total: formatTime(this.duration), line: pos.done, lines: this.lines }));
  };

  function median(v) {
    const s = (v || []).filter(function (x) { return x !== null; }).slice().sort(function (a, b) { return a - b; });
    return s.length ? s[Math.floor(s.length / 2)] : 0;
  }

  function cssVar(elm, name, fallback) {
    const v = getComputedStyle(elm).getPropertyValue(name).trim();
    return v || fallback;
  }

  /** Static layer of the timeline: phases, SNR curve, line quality, disturbances. */
  Player.prototype.drawTimeline = function () {
    const c = this.timeline;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(c.clientWidth * dpr));
    const h = Math.max(1, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) {
      c.width = w;
      c.height = h;
    }
    const layer = this.timelineLayer || (this.timelineLayer = document.createElement('canvas'));
    layer.width = w;
    layer.height = h;
    const g = layer.getContext('2d');
    const d = this.duration || 1, sync = this.sync;
    const x = function (t) { return t / d * w; };
    g.fillStyle = cssVar(this.root, '--owp-track', '#dde3e8');
    g.fillRect(0, 0, w, h);
    // Phases, as a band at the bottom.
    const band = Math.round(6 * dpr);
    const seg = function (a, b, color) {
      if (a === null || a === undefined || b <= a) return;
      g.fillStyle = color;
      g.fillRect(x(a), h - band, x(b) - x(a), band);
    };
    seg(sync.apt_start_s, sync.phasing_s !== null ? sync.phasing_s : sync.image_s, '#8a6fd1');
    seg(sync.phasing_s, sync.image_s, '#4f8fd6');
    seg(sync.image_s, sync.image_end_s, cssVar(this.root, '--owp-muted', '#7d8b96'));
    seg(sync.image_end_s, Math.min(d, sync.image_end_s + 5), '#d18a2f');
    // Line quality, as thin coloured bars under the curve.
    const tele = this.tele;
    const top = h - band - Math.round(5 * dpr);
    if (tele && tele.line_quality) {
      const lp = linePeriod(sync, this.lines);
      const q = tele.line_quality;
      const per = Math.max(1, Math.floor(q.length / w));
      for (let i = 0; i < q.length; i += per) {
        let worst = 1;
        for (let k = i; k < Math.min(q.length, i + per); ++k) worst = Math.min(worst, q[k]);
        g.fillStyle = qualityColor(worst);
        const xa = x(sync.image_s + i * lp);
        g.fillRect(xa, top, Math.max(1, x(sync.image_s + (i + per) * lp) - xa), Math.round(4 * dpr));
      }
    }
    // Local disturbances.
    this.zones.forEach(function (z) {
      g.fillStyle = 'rgba(230, 120, 0, 0.35)';
      g.fillRect(x(z.start_s), 0, Math.max(2, x(z.end_s) - x(z.start_s)), top);
    });
    // SNR curve, 0 … 40 dB.
    if (tele && tele.snr_db && tele.snr_db.length) {
      const s = tele.snr_db, iv = tele.interval_s || 1;
      const plotH = top - 4 * dpr;
      g.strokeStyle = cssVar(this.root, '--owp-accent', '#0b5cad');
      g.lineWidth = 1.5 * dpr;
      g.beginPath();
      for (let i = 0; i < s.length; ++i) {
        const yy = 2 * dpr + plotH * (1 - clamp(s[i], 0, 40) / 40);
        if (i === 0) g.moveTo(x((i + 0.5) * iv), yy);
        else g.lineTo(x((i + 0.5) * iv), yy);
      }
      g.stroke();
    }
    this.drawCursor(this.time());
  };

  Player.prototype.drawCursor = function (t) {
    const c = this.timeline;
    if (!this.timelineLayer || !c.width) return;
    const g = c.getContext('2d');
    g.drawImage(this.timelineLayer, 0, 0);
    const dpr = window.devicePixelRatio || 1;
    const xx = clamp(t / (this.duration || 1), 0, 1) * c.width;
    g.fillStyle = cssVar(this.root, '--owp-text', '#13202b');
    g.fillRect(Math.round(xx - dpr), 0, Math.max(2, Math.round(2 * dpr)), c.height);
  };

  /** Stops everything and frees the observers (switching to another chart). */
  /** True when the visitor turned the sound on. */
  Player.prototype.soundOn = function () { return !!this.audio && !this.audio.muted; };

  Player.prototype.destroy = function () {
    this.destroyed = true;
    if (this.frame !== null) window.cancelAnimationFrame(this.frame);
    if (this.audio) {
      this.audio.pause();
      this.audio.removeAttribute('src');
      this.audio.load();
    }
    if (this.observer) this.observer.disconnect();
    if (this.resizer) this.resizer.disconnect();
    if (this.onVisibility) document.removeEventListener('visibilitychange', this.onVisibility);
    this.root.textContent = '';
    this.root.classList.remove('owp-started', 'owp-playing');
  };

  const api = {
    create: function (container, options) {
      const p = new Player(container, options);
      p.load();
      return p;
    },
    Player: Player,
    strings: STRINGS,
    // Exposed for the tests (tests/web/test_player.js).
    util: { linePeriod: linePeriod, lineAt: lineAt, phaseAt: phaseAt, sampleAt: sampleAt, meterPosition: meterPosition,
      vuPosition: vuPosition, sLabel: sLabel, qualityColor: qualityColor, formatTime: formatTime,
      formatUtc: formatUtc, localZones: localZones },
  };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OwfaxPlayer = api;
})(typeof window !== 'undefined' ? window : this);
