/**
 * ADSB position extrapolation for timestamp synchronization
 */

const { calculateBistaticDelay, calculateBistaticDoppler } = require('./geometry');

const FT_TO_M = 0.3048;
const FTMIN_TO_FTPS = 1 / 60; // ft/min to ft/s

// How far a position may be projected, in seconds, before it is refused.
//
// This is a confidence cutoff on a constant-velocity model, not a property of
// the data. It was 5 s, chosen when every source refreshed positions about
// once a second. ADSBHub-fed sources do not: adsb.retina.fm refreshes in whole
// batches every 8.6 s, so each aircraft's age sweeps a 1-9 s sawtooth and a 5 s
// cutoff rejected the entire population on roughly 7 polls in 10. 10 s covers
// the whole sweep.
//
// The cost is the error a straight-line projection accumulates over the extra
// seconds. For an airliner in cruise that is small, since it is flying very
// nearly straight; it grows only while an aircraft is manoeuvring, and a
// standard-rate turn held for the full 10 s is the worst case.
const MAX_EXTRAPOLATION_S = 10.0;

// How old a position the detection matcher will still project, in seconds.
//
// Wider than MAX_EXTRAPOLATION_S because the matcher has no better option: it
// used to compare raw positions, so every second of age became error. adsb.retina.fm
// positions reach blah2-api 1-15 s old (median ~11 s on jn1, 2026-10-05), and a
// 10 s cutoff would drop about half of them. At 20 s a cruising airliner's
// straight-line error is still well inside the 2 km delay tolerance; a turning
// one may simply fail to match, which is the same outcome as dropping it.
const MATCH_MAX_POSITION_AGE_S = 20.0;

// Vertical rate in ft/min.
//
// geom_rate is GNSS-derived and preferred, but it reaches this code from
// neither source: ADSBHub's SBS feed has no such field, and the tar1090 proxy's
// converter does not pass adsb.lol's through. baro_rate is the pressure-derived
// equivalent, it is carried by both feeds and the converter does pass it, and
// the two differ by a few percent - far less than the ~750 ft gap between the
// two altitudes. Reading only geom_rate meant vel_up was always 0, so a
// climbing or descending aircraft got the Doppler of a level one.
function verticalRate(aircraft) {
  if (typeof aircraft.geom_rate === 'number') {
    return aircraft.geom_rate;
  }
  if (typeof aircraft.baro_rate === 'number') {
    return aircraft.baro_rate;
  }
  return 0;
}

function extrapolatePosition(aircraft, targetTimestamp, maxSeconds = MAX_EXTRAPOLATION_S) {
  const dt = targetTimestamp - (aircraft.timestamp || 0);

  if (Math.abs(dt) > maxSeconds || !aircraft.gs || aircraft.track === null || aircraft.track === undefined) {
    return null;
  }

  const velocityMs = aircraft.gs * 0.514444;
  const trackRad = aircraft.track * Math.PI / 180;

  const dx = velocityMs * Math.sin(trackRad) * dt;
  const dy = velocityMs * Math.cos(trackRad) * dt;
  // vertical rate is in ft/min; keep altitude in feet to match alt_geom units
  const dz = verticalRate(aircraft) * FTMIN_TO_FTPS * dt;

  const latRad = aircraft.lat * Math.PI / 180;
  const newLat = aircraft.lat + (dy / 111320);
  const newLon = aircraft.lon + (dx / (111320 * Math.cos(latRad)));
  // alt stays in feet (matching alt_geom); geometry.js handles ft->m conversion.
  //
  // Fall back to alt_baro. Reading only alt_geom silently discarded most of
  // the sky: adsb.lol populates alt_geom for 35 of 87 aircraft, and ADSBHub's
  // SBS feed carries no geometric altitude at all, so every aircraft from
  // adsb.retina.fm collapsed to 0 ft. Zero is falsy, so bistatic.js rejected
  // it and geometry.js turned the rejection into a delay of exactly 0 - a
  // target that looks present and is useless. bistatic.js has its own
  // `alt_geom ?? alt_baro`, but it can never fire, because this line has
  // already replaced the missing value with 0 and the result is handed over
  // labelled alt_geom.
  //
  // Barometric and geometric altitude differ by tens to hundreds of feet,
  // which is negligible against a bistatic delay measured over 100+ km.
  // alt_baro is the string "ground" for surface movements, which must not be
  // added to a number.
  const baseAlt = typeof aircraft.alt_geom === 'number'
    ? aircraft.alt_geom
    : (typeof aircraft.alt_baro === 'number' ? aircraft.alt_baro : 0);
  const newAlt = baseAlt + dz;

  return {
    lat: newLat,
    lon: newLon,
    alt: newAlt
  };
}

/**
 * An aircraft record moved to where it was at targetTimestamp, for the
 * detection matcher.
 *
 * The matcher compares a detection against each aircraft's expected delay and
 * Doppler. Comparing the position as reported means comparing against where
 * the aircraft was seen_pos seconds ago, and near the baseline an airliner's
 * Doppler changes by ~5 Hz/s, so a few seconds of age alone exceeds the 5 Hz
 * tolerance (live on jn1, 2026-10-05: an SNR 20 overflight never matched).
 *
 * The result keeps every field bistatic.js reads. Altitude goes in alt_geom,
 * as extrapolatePosition already resolves the alt_baro fallback, and the
 * vertical rate goes in geom_rate, the only rate field bistatic.js reads, so
 * a climbing aircraft gets the Doppler of a climbing one.
 *
 * @param {object} aircraft Record carrying `timestamp`: position time, epoch s.
 * @param {number} targetTimestamp Detection time, epoch s.
 * @returns {object|null} The moved record with `position_age` (s), or null
 *   when the position is too old or the record lacks speed or heading.
 */
function projectForMatch(aircraft, targetTimestamp) {
  const pos = extrapolatePosition(aircraft, targetTimestamp, MATCH_MAX_POSITION_AGE_S);
  if (!pos) {
    return null;
  }
  return {
    ...aircraft,
    lat: pos.lat,
    lon: pos.lon,
    alt_geom: pos.alt,
    geom_rate: verticalRate(aircraft),
    position_age: targetTimestamp - aircraft.timestamp
  };
}

/**
 * The aircraft the detection matcher should compare a frame against.
 *
 * Each record is projected to the detection's time, and dropped if it cannot
 * be (too old, or no speed or heading, without which bistatic.js cannot give
 * a Doppler and it could never have matched). With no detection time, or a
 * record carrying no position time, the record is used as reported: the age
 * is unknown, so there is nothing to correct by.
 *
 * Done once per frame, not per detection, since it does not depend on the
 * detection.
 *
 * @param {object[]} aircraft Records from stampPositionTimes().
 * @param {number|undefined} detectionTimestamp Detection time, epoch s.
 * @returns {object[]}
 */
function aircraftAtTimestamp(aircraft, detectionTimestamp) {
  if (typeof detectionTimestamp !== 'number' || !isFinite(detectionTimestamp)) {
    return aircraft;
  }
  const result = [];
  for (const ac of aircraft) {
    if (typeof ac.timestamp !== 'number') {
      result.push(ac);
      continue;
    }
    const projected = projectForMatch(ac, detectionTimestamp);
    if (projected) {
      result.push(projected);
    }
  }
  return result;
}

/**
 * Give each aircraft in a tar1090 aircraft.json payload its position time,
 * `timestamp`, in epoch seconds.
 *
 * seen_pos is measured from the payload's own `now`, which the tar1090 proxy
 * sets to when it fetched the data, not when it served it. A cached payload
 * served 10 s later still says `now` = fetch time. Measuring seen_pos from
 * our own clock instead would make every cached position look that much
 * younger than it is. The local clock is the fallback for a payload with no
 * `now`.
 *
 * @param {object} payload Parsed aircraft.json.
 * @param {number} receivedAt Local time the payload arrived, epoch s.
 * @returns {object[]} Copies of payload.aircraft with `timestamp` set.
 */
function stampPositionTimes(payload, receivedAt) {
  const now = typeof payload.now === 'number' ? payload.now : receivedAt;
  return (payload.aircraft || []).map((ac) => ({
    ...ac,
    timestamp: now - (ac.seen_pos || 0)
  }));
}

function extrapolateAdsbData(adsbData, detectionTimestamp, rxPos, txPos, frequency) {
  const synchronized = {};
  let stats = { total: 0, extrapolated: 0, failed: 0 };

  for (const [hexId, aircraft] of Object.entries(adsbData)) {
    stats.total++;

    const extrapolatedPos = extrapolatePosition(aircraft, detectionTimestamp);

    if (extrapolatedPos) {
      stats.extrapolated++;

      const syncAircraft = { ...aircraft };
      syncAircraft.lat = extrapolatedPos.lat;
      syncAircraft.lon = extrapolatedPos.lon;
      syncAircraft.alt_geom = extrapolatedPos.alt;
      syncAircraft.timestamp = detectionTimestamp;
      syncAircraft.extrapolated = true;

      if (rxPos && txPos) {
        syncAircraft.delay = calculateBistaticDelay(extrapolatedPos, rxPos, txPos);

        if (frequency) {
          const velocity = {
            gs: aircraft.gs || 0,
            track: aircraft.track || 0,
            geom_rate: verticalRate(aircraft)
          };
          syncAircraft.doppler = calculateBistaticDoppler(
            extrapolatedPos, velocity, rxPos, txPos, frequency
          );
        }
      }

      synchronized[hexId] = syncAircraft;
    } else {
      stats.failed++;
      synchronized[hexId] = aircraft;
    }
  }

  if (stats.total > 0) {
    const successRate = (stats.extrapolated / stats.total * 100).toFixed(1);
    console.log(`ADSB extrapolation: ${stats.extrapolated}/${stats.total} ` +
                `(${successRate}%) succeeded, ${stats.failed} failed`);
  }

  return synchronized;
}

module.exports = {
  MAX_EXTRAPOLATION_S,
  MATCH_MAX_POSITION_AGE_S,
  verticalRate,
  extrapolatePosition,
  projectForMatch,
  aircraftAtTimestamp,
  stampPositionTimes,
  extrapolateAdsbData
};
