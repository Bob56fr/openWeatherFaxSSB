// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Showcase packages of the station (owfaxd --vitrine), replayed with the player of
// the web showcase before they are published (PLY-025). Plain ES2017.

'use strict';

(function () {
  const API = '/api/v1/vitrine';
  const TEXT = {
    fr: {
      back: '← Tableau de bord', title: 'Vitrine : rediffusion',
      intro: 'Les cartes préparées pour la vitrine web, rejouées comme sur le site : ' +
        'vérifiez une carte avant sa publication.',
      chain: 'Enchaîner les cartes (lecture continue)',
      empty: 'Aucun paquet. Préparez-en avec « owfax-vitrine package » et lancez owfaxd ' +
        'avec --vitrine <dossier>.',
      offline: 'Pas de connexion au récepteur.',
      publish: 'Publiable', refused: 'Non publiée : {reasons}',
      'reason.no_image': 'pas d’image', 'reason.incomplete': 'carte incomplète',
      'reason.low_quality': 'qualité insuffisante',
      'reason.local_disturbance': 'perturbation locale',
      'reason.station_not_allowed': 'station non autorisée',
      quality: 'qualité {q} %', disturbed: 'perturbée {p} %',
    },
    en: {
      back: '← Dashboard', title: 'Showcase: replay',
      intro: 'The charts prepared for the web showcase, replayed as on the site: ' +
        'check a chart before it is published.',
      chain: 'Chain the charts (continuous play)',
      empty: 'No package. Make some with "owfax-vitrine package" and run owfaxd with ' +
        '--vitrine <directory>.',
      offline: 'No connection to the receiver.',
      publish: 'Publishable', refused: 'Not published: {reasons}',
      'reason.no_image': 'no image', 'reason.incomplete': 'incomplete chart',
      'reason.low_quality': 'quality too low',
      'reason.local_disturbance': 'local disturbance',
      'reason.station_not_allowed': 'station not allowed',
      quality: 'quality {q} %', disturbed: 'disturbed {p} %',
    },
  };

  function load(key, fallback) {
    try {
      const v = window.localStorage.getItem('owfax.' + key);
      return v === null ? fallback : v;
    } catch (e) {
      return fallback;
    }
  }

  const browser = (navigator.language || 'fr').toLowerCase().indexOf('fr') === 0 ? 'fr' : 'en';
  const lang = load('lang', browser) === 'en' ? 'en' : 'fr';
  const theme = load('theme', 'auto');
  if (theme !== 'auto') document.documentElement.setAttribute('data-theme', theme);
  document.documentElement.lang = lang;

  function t(key, vars) {
    let s = TEXT[lang][key] || key;
    Object.keys(vars || {}).forEach(function (k) { s = s.split('{' + k + '}').join(String(vars[k])); });
    return s;
  }

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = text;
    return e;
  }

  document.querySelectorAll('[data-text]').forEach(function (e) {
    e.textContent = t(e.getAttribute('data-text'));
  });
  document.title = t('title') + ' — openWeatherFaxSSB';

  const playerBox = document.getElementById('player');
  const list = document.getElementById('packages');
  const chain = document.getElementById('chain');
  let packages = [];
  let player = null;
  let current = '';

  function base(id) { return API + '/' + encodeURIComponent(id) + '/'; }

  function open(id, auto) {
    const sound = !!player && player.soundOn();
    if (player) player.destroy();
    current = id;
    player = window.OwfaxPlayer.create(playerBox, {
      base: base(id),
      lang: lang,
      auto: !!auto,
      sound: !!auto && sound,
      onEnded: function () {
        if (!chain.checked) return;
        const i = packages.findIndex(function (m) { return m.chart_id === current; });
        // Newest first in the list: the playlist goes on with the next older one.
        const next = packages[(i + 1) % packages.length];
        if (next) open(next.chart_id, true);
      },
    });
    list.querySelectorAll('button').forEach(function (b) {
      b.setAttribute('aria-current', String(b.getAttribute('data-id') === id));
    });
    if (window.location.hash.slice(1) !== id) {
      window.history.replaceState(null, '', '#' + encodeURIComponent(id));
    }
    playerBox.scrollIntoView({ block: 'start', behavior: 'smooth' });
  }

  function card(m) {
    const li = el('li');
    const b = el('button');
    b.type = 'button';
    b.setAttribute('data-id', m.chart_id);
    const img = el('img');
    img.loading = 'lazy';
    img.alt = '';
    img.src = base(m.chart_id) + 'thumb.webp';
    b.appendChild(img);
    const cap = el('span', 'vt-caption');
    const st = m.station || {};
    const start = (m.time && m.time.start_utc) || '';
    cap.appendChild(el('span', 'vt-when', start.replace('T', ' ').slice(0, 16) + ' UTC'));
    cap.appendChild(el('span', null, [st.callsign, st.assigned_khz ? st.assigned_khz + ' kHz' : '']
      .filter(Boolean).join(' · ')));
    const facts = [];
    if (m.quality && m.quality.mean !== undefined) {
      facts.push(t('quality', { q: Math.round(m.quality.mean * 100) }));
    }
    const d = m.disturbances;
    if (d && d.local_fraction > 0) facts.push(t('disturbed', { p: Math.round(d.local_fraction * 100) }));
    if (m.path && m.path.distance_km !== undefined) facts.push(m.path.distance_km + ' km');
    cap.appendChild(el('span', 'vt-facts', facts.join(' · ')));
    const sel = m.selection || {};
    const reasons = (sel.reasons || []).map(function (r) { return t('reason.' + r); }).join(', ');
    cap.appendChild(el('span', 'vt-verdict ' + (sel.publish ? 'ok' : 'refused'),
      sel.publish ? t('publish') : t('refused', { reasons: reasons })));
    b.appendChild(cap);
    b.addEventListener('click', function () { open(m.chart_id, false); });
    li.appendChild(b);
    return li;
  }

  async function refresh() {
    const empty = document.getElementById('empty');
    try {
      const r = await fetch(API, { cache: 'no-store' });
      packages = r.ok ? await r.json() : [];
      empty.textContent = t('empty');
    } catch (e) {
      packages = [];
      empty.textContent = t('offline');
    }
    list.textContent = '';
    packages.forEach(function (m) { list.appendChild(card(m)); });
    empty.hidden = packages.length > 0;
    const wanted = decodeURIComponent(window.location.hash.slice(1));
    if (wanted && packages.some(function (m) { return m.chart_id === wanted; })) open(wanted, false);
  }

  refresh();
})();
