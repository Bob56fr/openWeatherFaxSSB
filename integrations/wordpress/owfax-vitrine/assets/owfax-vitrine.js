// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Starts the replay player on every .owfax-player of the page. One chart: nothing
// loads before the visitor presses Play. Several (playlist): the newest starts by
// itself, muted, when it comes into view, then the others follow (PLY-007, PLY-021).
// Plain ES2017; reads only static files from uploads/owfax/ (VIT-002).

'use strict';

(function () {
  function start(box) {
    let bases;
    try {
      bases = JSON.parse(box.getAttribute('data-bases') || '[]');
    } catch (e) {
      return;
    }
    if (!Array.isArray(bases) || bases.length === 0 || !window.OwfaxPlayer) return;
    const lang = box.getAttribute('data-lang') === 'en' ? 'en' : 'fr';
    const rate = Number(box.getAttribute('data-rate')) || 1;
    const auto = box.getAttribute('data-auto') === '1';
    let player = null;
    let index = 0;

    function open(i, autoStart) {
      // The sound stays on from one chart to the next if the visitor turned it on.
      const sound = !!player && player.soundOn();
      if (player) player.destroy();
      index = i;
      player = window.OwfaxPlayer.create(box, {
        base: bases[i],
        lang: lang,
        rate: rate,
        auto: autoStart,
        sound: autoStart && sound,
        onEnded: bases.length > 1 ? function () { open((index + 1) % bases.length, true); } : null,
      });
    }
    open(0, auto);
  }

  function all() {
    document.querySelectorAll('.owfax-player[data-bases]').forEach(start);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', all);
  else all();
})();
