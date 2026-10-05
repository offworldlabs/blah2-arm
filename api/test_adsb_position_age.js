// Tests that the detection matcher compares against where each aircraft was at
// the detection's time, not where it was last reported.
//
// The failure this guards, seen live on jonathan-node-1 on 2026-10-05: an
// airliner passing the node at SNR 11-20 never matched ADS-B, because its
// positions from adsb.retina.fm were 2-15 s old and near the baseline its
// Doppler changed by ~5 Hz/s, so the raw position missed the 5 Hz tolerance by
// 4-23 Hz. The overflight below reproduces those numbers.
//
// The aircraft's true position at detection time is computed with a spherical
// destination-point formula, independent of extrapolatePosition's flat-earth
// step, so the test does not just check the projection against itself.
//
// Run with: node test_adsb_position_age.js

const bistatic = require('./bistatic.js');
const {
  MATCH_MAX_POSITION_AGE_S,
  MAX_EXTRAPOLATION_S,
  projectForMatch,
  aircraftAtTimestamp,
  stampPositionTimes
} = require('./lib/extrapolation');

let passed = 0;
let failed = 0;

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`  PASS: ${name}`);
  } else {
    failed++;
    console.error(`  FAIL: ${name}${detail ? `  <- ${detail}` : ''}`);
  }
}

// jonathan-node-1's real geometry (WGBY-TV, 213 MHz).
const rx = { latitude: 42.23719160239999, longitude: -72.6835149638518, altitude: 79.64 };
const tx = { latitude: 42.241528, longitude: -72.648361, altitude: 619.2 };
const FC = 213e6;
const DELAY_TOL = 2.0;
const DOPPLER_TOL = 5.0;

const EARTH_R = 6371008.8;
const KNOTS_TO_MS = 0.514444;

// Spherical destination point: start, bearing (deg), distance (m).
function destination(lat, lon, bearingDeg, dist) {
  const p1 = lat * Math.PI / 180;
  const l1 = lon * Math.PI / 180;
  const t = bearingDeg * Math.PI / 180;
  const a = dist / EARTH_R;
  const p2 = Math.asin(Math.sin(p1) * Math.cos(a) + Math.cos(p1) * Math.sin(a) * Math.cos(t));
  const l2 = l1 + Math.atan2(Math.sin(t) * Math.sin(a) * Math.cos(p1),
    Math.cos(a) - Math.sin(p1) * Math.sin(p2));
  return { lat: p2 * 180 / Math.PI, lon: l2 * 180 / Math.PI };
}

// A jet flying west at 38000 ft, 460 kt, passing ~8 km north of the receiver.
// `truth` is where it is at the detection; `reported(age)` is the record the
// feed delivers, last positioned `age` seconds earlier.
const DET_TS = 1791211500.25; // epoch s
const truth = { hex: 'aaaaaa', lat: 42.309, lon: -72.6, alt_baro: 38000, gs: 460, track: 270 };
function reported(age) {
  const back = destination(truth.lat, truth.lon, 90, truth.gs * KNOTS_TO_MS * age);
  return { ...truth, lat: back.lat, lon: back.lon, timestamp: DET_TS - age };
}

const detDelay = bistatic.computeBistaticDelay(truth, rx, tx);
const detDoppler = bistatic.computeBistaticDoppler(truth, rx, tx, FC);

function residuals(ac) {
  return {
    delay: detDelay - bistatic.computeBistaticDelay(ac, rx, tx),
    doppler: detDoppler - bistatic.computeBistaticDoppler(ac, rx, tx, FC)
  };
}

console.log(`overflight: detection at ${detDelay.toFixed(2)} km, ${detDoppler.toFixed(1)} Hz`);

console.log('\nthe raw reported position misses, as it did live:');
for (const age of [2, 5]) {
  const r = residuals(reported(age));
  check(`${age} s old, raw: Doppler off by more than the ${DOPPLER_TOL} Hz tolerance`,
    Math.abs(r.doppler) > DOPPLER_TOL, `${r.doppler.toFixed(1)} Hz`);
}

console.log('\nprojected to the detection time, it matches:');
for (const age of [2, 5, 10, 15]) {
  const p = projectForMatch(reported(age), DET_TS);
  const r = p && residuals(p);
  check(`${age} s old, projected: delay within 0.05 km, Doppler within 0.5 Hz`,
    p && Math.abs(r.delay) < 0.05 && Math.abs(r.doppler) < 0.5,
    p ? `${r.delay.toFixed(3)} km, ${r.doppler.toFixed(2)} Hz` : 'dropped');
  check(`${age} s old, projected: position_age is ${age}`,
    p && Math.abs(p.position_age - age) < 1e-6, p && p.position_age);
}

console.log('\nage limit:');
check('the matcher limit is wider than the overlay limit',
  MATCH_MAX_POSITION_AGE_S > MAX_EXTRAPOLATION_S);
check('15 s old is kept (the overlay would drop it)',
  projectForMatch(reported(15), DET_TS) !== null);
check(`older than ${MATCH_MAX_POSITION_AGE_S} s is dropped`,
  projectForMatch(reported(MATCH_MAX_POSITION_AGE_S + 1), DET_TS) === null);
{
  // A position newer than the detection (the detection waited for its CPI to
  // finish processing) is projected backwards, not refused.
  const newer = { ...truth, timestamp: DET_TS + 1 };
  const p = projectForMatch(newer, DET_TS);
  check('a position 1 s newer than the detection is projected back',
    p && Math.abs(p.position_age + 1) < 1e-6);
}

console.log('\nfields bistatic.js needs:');
{
  const p = projectForMatch({ ...reported(5), baro_rate: 1800 }, DET_TS);
  check('baro_rate reaches geom_rate, the only rate field bistatic.js reads',
    p && p.geom_rate === 1800);
  check('altitude moves with the climb (5 s at 1800 ft/min = +150 ft)',
    p && Math.abs(p.alt_geom - 38150) < 1e-6, p && p.alt_geom);
  check('alt_baro only: altitude lands in alt_geom', p && typeof p.alt_geom === 'number');
}
check('no ground speed: dropped (bistatic.js could give no Doppler either)',
  projectForMatch({ ...reported(5), gs: undefined }, DET_TS) === null);
check('no track: dropped',
  projectForMatch({ ...reported(5), track: undefined }, DET_TS) === null);

console.log('\naircraftAtTimestamp:');
{
  const list = [reported(5), reported(MATCH_MAX_POSITION_AGE_S + 5), { ...truth, hex: 'nostamp' }];
  const out = aircraftAtTimestamp(list, DET_TS);
  check('projects stamped records and drops ones too old to project',
    out.length === 2 && out[0].position_age === 5);
  check('a record with no position time is passed through unchanged',
    out[1] === list[2]);
  check('no detection timestamp: the list is returned as reported',
    aircraftAtTimestamp(list, undefined) === list);
  check('order is preserved, so ties still go to the first aircraft',
    out.map((a) => a.hex).join() === 'aaaaaa,nostamp');
}

console.log('\nstampPositionTimes:');
{
  // The proxy served a cached payload fetched at now=1000; it reached us at
  // 1012. A position seen 3 s before the fetch is 15 s old, not 3.
  const payload = { now: 1000, aircraft: [{ hex: 'a', seen_pos: 3 }, { hex: 'b' }] };
  const out = stampPositionTimes(payload, 1012);
  check('position time = payload now - seen_pos, not our clock - seen_pos',
    out[0].timestamp === 997);
  check('no seen_pos: positioned at payload now', out[1].timestamp === 1000);
  check('the payload records are not mutated', payload.aircraft[0].timestamp === undefined);
  check('no payload now: falls back to the arrival time',
    stampPositionTimes({ aircraft: [{ seen_pos: 2 }] }, 1012)[0].timestamp === 1010);
  check('no aircraft array: empty list', stampPositionTimes({ now: 1 }, 1).length === 0);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
