/**
 * ADSB position extrapolation for timestamp synchronization
 */

const { calculateBistaticDelay, calculateBistaticDoppler } = require('./geometry');

const FT_TO_M = 0.3048;
const FTMIN_TO_FTPS = 1 / 60; // ft/min to ft/s

function extrapolatePosition(aircraft, targetTimestamp) {
  const dt = targetTimestamp - (aircraft.timestamp || 0);

  if (Math.abs(dt) > 5.0 || !aircraft.gs || aircraft.track === null || aircraft.track === undefined) {
    return null;
  }

  const velocityMs = aircraft.gs * 0.514444;
  const trackRad = aircraft.track * Math.PI / 180;

  const dx = velocityMs * Math.sin(trackRad) * dt;
  const dy = velocityMs * Math.cos(trackRad) * dt;
  // geom_rate is in ft/min; keep altitude in feet to match alt_geom units
  const dz = (aircraft.geom_rate || 0) * FTMIN_TO_FTPS * dt;

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
            geom_rate: aircraft.geom_rate || 0
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
  extrapolatePosition,
  extrapolateAdsbData
};
