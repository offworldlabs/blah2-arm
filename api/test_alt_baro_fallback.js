#!/usr/bin/env node
/**
 * The ADS-B truth path must not require geometric altitude.
 *
 * ADSBHub's SBS feed carries no alt_geom at all, and adsb.lol populates it for
 * well under half its aircraft. Reading only alt_geom put those aircraft at
 * 0 ft, which bistatic.js rejects as falsy, which geometry.js turns into a
 * delay of exactly 0 - a truth target that looks present and is useless.
 *
 * Run: node api/test_alt_baro_fallback.js
 */

const assert = require('assert');
const { extrapolateAdsbData } = require('./lib/extrapolation');

const rxPos = { lat: 42.237, lon: -72.684, alt: 80 };
const txPos = { lat: 42.5, lon: -72.0, alt: 300 };
const frequency = 100e6;

function truthFor(aircraft) {
  const now = Date.now() / 1000;
  const data = { abc123: { hex: 'abc123', timestamp: now - 1, lat: 42.4, lon: -72.9, gs: 400, track: 90, ...aircraft } };
  const synchronized = extrapolateAdsbData(data, now, rxPos, txPos, frequency);
  return synchronized.abc123;
}

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`  PASS  ${name}`);
  } catch (err) {
    failures++;
    console.log(`  FAIL  ${name}\n        ${err.message}`);
  }
}

console.log('ADS-B truth altitude fallback');

check('geometric altitude is used when present', () => {
  const ac = truthFor({ alt_geom: 24850, alt_baro: 24000 });
  assert.ok(ac.delay > 0, `expected a delay, got ${ac.delay}`);
  // alt_geom wins, so the delay must differ from the alt_baro-derived one.
  const baroOnly = truthFor({ alt_baro: 24000 });
  assert.notStrictEqual(ac.delay, baroOnly.delay, 'alt_geom must take precedence over alt_baro');
});

check('barometric altitude is used when geometric is missing', () => {
  // An adsb.retina.fm aircraft: SBS carries alt_baro and nothing else.
  const ac = truthFor({ alt_baro: 24000 });
  assert.ok(ac.delay > 0, `expected a nonzero delay, got ${ac.delay}`);
  assert.ok(ac.doppler !== undefined, 'doppler should still be computed');
});

check('baro and geometric altitude give near-identical delay', () => {
  // The whole justification for the fallback: the difference is negligible
  // against a bistatic delay measured over tens of kilometres.
  const geom = truthFor({ alt_geom: 24850 });
  const baro = truthFor({ alt_baro: 24000 });
  const diff = Math.abs(geom.delay - baro.delay);
  assert.ok(diff < 1.0, `delay differed by ${diff.toFixed(3)} km, expected under 1 km`);
});

check('a surface movement does not become NaN', () => {
  // alt_baro is the string "ground" for aircraft on the surface.
  const ac = truthFor({ alt_baro: 'ground' });
  assert.ok(!Number.isNaN(ac.delay), 'delay must not be NaN');
  assert.strictEqual(ac.delay, 0, 'a grounded aircraft yields no usable delay');
});

check('no altitude at all still yields no delay', () => {
  const ac = truthFor({});
  assert.strictEqual(ac.delay, 0, 'unchanged behaviour when neither altitude is present');
});

check('a full adsb-service aircraft produces usable truth', () => {
  // Exactly the field set adsb.retina.fm serves: no alt_geom, no geom_rate.
  const ac = truthFor({ alt_baro: 24000, gs: 408, track: 236, seen_pos: 2.0 });
  assert.ok(ac.extrapolated, 'must extrapolate');
  assert.ok(ac.delay > 0, `expected a usable delay, got ${ac.delay}`);
});


// ---------------------------------------------------------------------------
// Extrapolation window
// ---------------------------------------------------------------------------

const { MAX_EXTRAPOLATION_S, extrapolatePosition } = require('./lib/extrapolation');

console.log('\nExtrapolation window');

check('the window covers adsb-service\'s 1-9 s batch sawtooth', () => {
  assert.ok(MAX_EXTRAPOLATION_S >= 9.0, `window is ${MAX_EXTRAPOLATION_S}s, batches reach 9s`);
  const now = Date.now() / 1000;
  const ac = { lat: 42.4, lon: -72.9, alt_baro: 24000, gs: 400, track: 90, timestamp: now - 8 };
  assert.ok(extrapolatePosition(ac, now) !== null, 'an 8 s old position must project');
});

check('a position past the window is still refused', () => {
  const now = Date.now() / 1000;
  const ac = { lat: 42.4, lon: -72.9, alt_baro: 24000, gs: 400, track: 90, timestamp: now - 11 };
  assert.strictEqual(extrapolatePosition(ac, now), null, 'an 11 s old position must be refused');
});

check('the worst-case cost of the wider window is bounded', () => {
  // A straight-line projection is wrong only while the aircraft manoeuvres.
  // Worst realistic case: a standard-rate turn (3 deg/s) held for the whole
  // window. Compare where we say it is against where it actually would be.
  const v = 200;                        // m/s, ~390 kt
  const omega = 3 * Math.PI / 180;      // standard rate, rad/s
  const r = v / omega;

  function errorAfter(t) {
    const straight = { x: 0, y: v * t };                       // our projection
    const turned = { x: r * (1 - Math.cos(omega * t)),         // the truth
                     y: r * Math.sin(omega * t) };
    return Math.hypot(straight.x - turned.x, straight.y - turned.y);
  }

  const at5 = errorAfter(5);
  const at10 = errorAfter(10);
  console.log(`        worst-case turn error: ${at5.toFixed(0)} m at 5 s, ${at10.toFixed(0)} m at 10 s`);
  assert.ok(at10 < 1000, `10 s worst case is ${at10.toFixed(0)} m, expected under 1 km`);
});

console.log(failures === 0 ? '\nAll checks passed' : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
