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
      'pub.off': 'Publication vitrine désactivée : la station n’envoie rien.',
      'pub.active': 'Publication vitrine active vers {site}',
      'pub.stopped': 'Publication vitrine arrêtée depuis {when} UTC (vers {site})',
      'pub.pending': '{n} en attente', 'pub.next': 'prochain essai {when} UTC',
      'pub.refused': '{n} refusée(s) par le site', 'pub.last': 'dernière envoyée {when} UTC',
      'pub.error': 'dernier échec : {e}',
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
      'pub.off': 'Showcase publication off: the station sends nothing.',
      'pub.active': 'Showcase publication active towards {site}',
      'pub.stopped': 'Showcase publication stopped since {when} UTC (towards {site})',
      'pub.pending': '{n} waiting', 'pub.next': 'next try {when} UTC',
      'pub.refused': '{n} refused by the site', 'pub.last': 'last sent {when} UTC',
      'pub.error': 'last failure: {e}',
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

  function utc(s) { return String(s || '').replace('T', ' ').slice(0, 16); }

  // Publication to the web showcase (VIT-003): off unless owfax-vitrine send runs.
  async function publication() {
    const box = document.getElementById('publication');
    let s;
    try {
      const r = await fetch(API + '/publish', { cache: 'no-store' });
      s = r.ok ? await r.json() : { configured: false };
    } catch (e) {
      return;
    }
    // The sender writes its state every minute: older than 3 min, it is not running.
    const alive = s.active && s.now_unix - s.updated_unix < 180;
    const parts = [];
    let state = 'off';
    if (!s.configured || !s.site) {
      parts.push(t('pub.off'));
    } else {
      state = alive ? 'on' : 'off';
      parts.push(alive ? t('pub.active', { site: s.site })
        : t('pub.stopped', { site: s.site, when: utc(s.updated_utc) }));
      if (s.pending) parts.push(t('pub.pending', { n: s.pending }));
      if (s.pending && s.next_try_utc) parts.push(t('pub.next', { when: utc(s.next_try_utc) }));
      if (s.refused) parts.push(t('pub.refused', { n: s.refused }));
      if (s.last_sent) parts.push(t('pub.last', { when: utc(s.last_sent.utc) }));
      if (s.last_error && s.pending) parts.push(t('pub.error', { e: s.last_error }));
    }
    box.textContent = parts.join(' · ');
    box.setAttribute('data-state', state);
    box.hidden = false;
  }

  refresh();
  publication();
  window.setInterval(publication, 60000);
})();
