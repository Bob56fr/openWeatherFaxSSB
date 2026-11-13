// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Tests of the replay player's timing logic (web/vitrine/owfax-player.js,
// docs/VITRINE-WEB.md PLY-001/002/010). Run with: node tests/web/test_player.js

'use strict';

const assert = require('assert');
const path = require('path');
const P = require(path.join(__dirname, '..', '..', 'web', 'vitrine', 'owfax-player.js'));
const u = P.util;

let failures = 0;
function test(name, fn) {
  try {
    fn();
    console.log('PASS', name);
  } catch (e) {
    failures += 1;
    console.log('FAIL', name, '\n ', e.message);
  }
}

// A real package: APT at 2 s, phasing at 7 s, 1295 lines from 36.847 s.
const sync = { audio_duration_s: 696.344, apt_start_s: 2.0, phasing_s: 7.0, image_s: 36.847,
  image_end_s: 684.344, line_period_s: 0.499998 };
const lines = 1295;

test('the line follows the sound, to the line (PLY-001)', function () {
  assert.deepStrictEqual(u.lineAt(sync, lines, 0), { done: 0, frac: 0 });
  assert.deepStrictEqual(u.lineAt(sync, lines, 36.8), { done: 0, frac: 0 });
  // Every line n starts at image_s + n × period: n lines are complete at that time.
  const lp = u.linePeriod(sync, lines);
  assert.ok(Math.abs(lp - sync.line_period_s) < 1e-5);
  for (const n of [0, 1, 2, 100, 647, 1000, 1294]) {
    const t = sync.image_s + n * lp;
    assert.strictEqual(u.lineAt(sync, lines, t + 1e-6).done, n, 'line ' + n);
    assert.strictEqual(u.lineAt(sync, lines, t - 1e-6).done, Math.max(0, n - 1), 'before ' + n);
  }
  const mid = u.lineAt(sync, lines, sync.image_s + 10.25 * lp);
  assert.strictEqual(mid.done, 10);
  assert.ok(Math.abs(mid.frac - 0.25) < 1e-6);
  assert.deepStrictEqual(u.lineAt(sync, lines, sync.image_end_s), { done: lines, frac: 0 });
  assert.deepStrictEqual(u.lineAt(sync, lines, 1e6), { done: lines, frac: 0 });
});

test('the speed does not change the line: it only depends on the sound position', function () {
  // At ×10 the browser moves currentTime ten times faster; the mapping is the same.
  const t = 400.123;
  const n = Math.floor((t - sync.image_s) / u.linePeriod(sync, lines));
  assert.strictEqual(u.lineAt(sync, lines, t).done, n);
});

test('phases of the transmission (PLY-002)', function () {
  const d = sync.audio_duration_s;
  assert.strictEqual(u.phaseAt(sync, 0.5, d), 'wait');
  assert.strictEqual(u.phaseAt(sync, 2.0, d), 'apt');
  assert.strictEqual(u.phaseAt(sync, 6.9, d), 'apt');
  assert.strictEqual(u.phaseAt(sync, 7.0, d), 'phasing');
  assert.strictEqual(u.phaseAt(sync, 36.9, d), 'image');
  assert.strictEqual(u.phaseAt(sync, 684.4, d), 'stop');
  assert.strictEqual(u.phaseAt(sync, d, d), 'end');
  // Chart caught in progress: no APT, no phasing.
  const late = { apt_start_s: null, phasing_s: null, image_s: 0, image_end_s: 86, line_period_s: 0.5 };
  assert.strictEqual(u.phaseAt(late, 0, 98), 'image');
  assert.strictEqual(u.phaseAt(late, 90, 98), 'stop');
});

test('per-second telemetry is interpolated between second centres', function () {
  const s = [0, 10, 20];
  assert.strictEqual(u.sampleAt(s, 0, 1), 0);
  assert.strictEqual(u.sampleAt(s, 0.5, 1), 0);
  assert.strictEqual(u.sampleAt(s, 1.0, 1), 5);
  assert.strictEqual(u.sampleAt(s, 1.5, 1), 10);
  assert.strictEqual(u.sampleAt(s, 99, 1), 20);
  assert.strictEqual(u.sampleAt([], 1, 1), null);
  assert.strictEqual(u.sampleAt(undefined, 1, 1), null);
});

test('S-meter scale and labels (PLY-010)', function () {
  assert.strictEqual(u.meterPosition(-54), 0);
  assert.strictEqual(u.meterPosition(0), 0.6);
  assert.strictEqual(u.meterPosition(60), 1);
  assert.strictEqual(u.meterPosition(200), 1);
  assert.ok(u.meterPosition(-12) < u.meterPosition(-6));
  assert.strictEqual(u.sLabel(0), 'S9');
  assert.strictEqual(u.sLabel(-12), 'S7');
  assert.strictEqual(u.sLabel(-60), 'S0');
  assert.strictEqual(u.sLabel(20.4), 'S9+20');
  assert.strictEqual(u.vuPosition(-60), 0);
  assert.strictEqual(u.vuPosition(0), 1);
});

test('only local disturbances are hatched (PLY-006)', function () {
  assert.deepStrictEqual(u.localZones({}), []);
  assert.deepStrictEqual(u.localZones({ saturation: {} }), []);  // lot 1 packages
  const z = u.localZones({ disturbances: { zones: [
    { start_s: 40, end_s: 70, first_line: 6, last_line: 65, cause: 'overload', local: true },
    { start_s: 90, end_s: 120, first_line: 106, last_line: 165, cause: 'fading', local: false },
  ] } });
  assert.strictEqual(z.length, 1);
  assert.strictEqual(z[0].cause, 'overload');
});

test('formatting', function () {
  assert.strictEqual(u.formatTime(0), '0:00');
  assert.strictEqual(u.formatTime(696.3), '11:36');
  assert.strictEqual(u.formatUtc('2026-11-09T21:45:58Z'), '2026-11-09 21:45 UTC');
  assert.strictEqual(u.qualityColor(1), 'hsl(120, 75%, 42%)');
  assert.strictEqual(u.qualityColor(0.3), 'hsl(0, 75%, 42%)');
});

test('a replay is never called live, and always carries the warning (VIT-004)', function () {
  for (const lang of ['fr', 'en']) {
    const all = Object.values(P.strings[lang]).join(' ').toLowerCase();
    assert.ok(!/live|direct/.test(all), lang);
    assert.ok(P.strings[lang].badge && P.strings[lang].warning, lang);
  }
});

test('nothing is simulated: no random value in the player (VIT-005)', function () {
  const src = require('fs').readFileSync(path.join(__dirname, '..', '..', 'web', 'vitrine', 'owfax-player.js'), 'utf8');
  assert.ok(src.indexOf('Math.random') < 0);
});

test('every text exists in both languages', function () {
  const fr = Object.keys(P.strings.fr).sort(), en = Object.keys(P.strings.en).sort();
  assert.deepStrictEqual(fr, en);
});

if (failures) {
  console.log(failures + ' failed');
  process.exit(1);
}
console.log('all passed');
