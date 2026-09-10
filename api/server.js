const express = require('express');
const net = require("net");
const fs = require('fs');
const yaml = require('js-yaml');
const dns = require('dns');
const http = require('http');
const bistatic = require('./bistatic.js');
const { extrapolateAdsbData } = require('./lib/extrapolation');
const retuneLib = require('./lib/retune');

// parse config file
var config;
try {
  const file = process.argv[2];
  config = yaml.load(fs.readFileSync(file, 'utf8'));
} catch (e) {
  console.error('Error reading or parsing the YAML file:', e);
}

// ADS-B truth is the only thing here that needs a position; the radar, maps and
// IQ never read one. See location.js for what breaks without this check.
const { hasLocation } = require('./location.js');

function adsbTruthAvailable() {
  return !!(config && config.truth && config.truth.adsb
            && config.truth.adsb.enabled && hasLocation(config));
}

var stash_map = require('./stash/maxhold.js');
var stash_detection = require('./stash/detection.js');
var stash_iqdata = require('./stash/iqdata.js');
var stash_timing = require('./stash/timing.js');

// TCP client for forwarding detections to external tracker
let trackerSocket = null;
let trackerConnected = false;

function connectToTracker() {
  if (!config.network.tracker_forward?.enabled) return;

  const host = config.network.tracker_forward.host;
  const port = config.network.tracker_forward.port;

  trackerSocket = new net.Socket();

  trackerSocket.connect(port, host, () => {
    console.log(`Connected to tracker at ${host}:${port}`);
    trackerConnected = true;
  });

  trackerSocket.on('error', (err) => {
    console.error(`Tracker connection error: ${err.message}`);
    trackerConnected = false;
  });

  trackerSocket.on('close', () => {
    console.log('Tracker connection closed, reconnecting in 5s...');
    trackerConnected = false;
    setTimeout(connectToTracker, 5000);
  });
}

function forwardToTracker(data) {
  if (trackerConnected && trackerSocket) {
    // The newline is the frame delimiter, not decoration: retina-tracker reads
    // this socket by splitting its accumulated buffer on "\n" and parsing each
    // complete line. Without it every frame concatenates onto the last, the
    // buffer grows for the life of the process and not one detection is ever
    // parsed. The failure is silent at both ends.
    trackerSocket.write(data + '\n');
  }
}

// Initialize tracker connection
connectToTracker();

// constants
const PORT = config.network.ports.api;
// Use '::' for IPv6 dual-stack to support mDNS .local access from IPv6 clients
// (config.network.ip is used by blah2 core for internal TCP connections, not API listen)
const HOST = '::';
var map = '';
var detection = '';
var track = '';
var timestamp = '';
var timing = '';
var iqdata = '';
// Initialised, not bare `var`. Each of these accumulates a socket's bytes with
// `data_x = data_x + msg`, so an undefined seed prefixes the literal string
// "undefined" onto the first message after every start. On the detection socket
// that made JSON.parse throw, so the first frame was always discarded and
// /api/detection briefly served the garbage; the same applied to all six.
var data_map = '';
var data_detection = '';
var data_tracker = '';
var data_timestamp = '';
var data_timing = '';
var data_iqdata = '';
var capture = false;

// api server
const app = express();
// header on all requests
app.use(function(req, res, next) {
  res.header("Access-Control-Allow-Origin", "*");
  res.header('Cache-Control', 'private, no-cache, no-store, must-revalidate');
  res.header('Expires', '-1');
  res.header('Pragma', 'no-cache');
  next();
});
app.get('/', (req, res) => {
  res.send('Hello World');
});
app.get('/api/map', (req, res) => {
  res.send(map);
});
app.get('/api/detection', (req, res) => {
  res.send(detection);
});
app.get('/api/tracker', (req, res) => {
  res.send(track);
});
app.get('/api/timestamp', (req, res) => {
  res.send(timestamp);
});
app.get('/api/timing', (req, res) => {
  res.send(timing);
});
app.get('/api/iqdata', (req, res) => {
  res.send(iqdata);
});
app.get('/api/config', (req, res) => {
  res.json({
    truth: { adsb: { enabled: config.truth.adsb.enabled } }
  });
});

// stash API
app.get('/stash/map', (req, res) => {
  res.send(stash_map.get_data_map());
});
app.get('/stash/detection', (req, res) => {
  res.send(stash_detection.get_data_detection());
});
app.get('/stash/iqdata', (req, res) => {
  res.send(stash_iqdata.get_data_iqdata());
});
app.get('/stash/timing', (req, res) => {
  res.send(stash_timing.get_data_timing());
});

// read state of capture. blah2 polls this once a second to decide whether to
// write IQ. There is deliberately no endpoint to set it: the toggle used to be
// an unauthenticated mutating GET bound to the spacebar, and a single stray
// keypress filled a node's disk. Any replacement must be an explicit control
// (a button, a POST, and a visible recording indicator), not a bare keybinding.
app.get('/capture', (req, res) => {
  res.send(capture);
});

// live retune state: seeded from the config this stack booted with;
// generation 0 means "no live retune issued this boot" and blah2 must no-op
var retune = {
  generation: 0,
  fc: config.capture.fc,
  gainReductionA: config.capture.device.gainReduction[0],
  gainReductionB: config.capture.device.gainReduction[1],
  lnaState: config.capture.device.lnaState,
};
// last ack blah2 posted back, or null if none yet
var retuneStatus = null;
// set when blah2 gives up on the pending generation (see
// /capture/retune/reject). Until the next POST /capture/retune supersedes
// it, the GET below must stop offering that generation: blah2 retries a
// pending retune every poll until it succeeds, and its own attempt counter
// lives in process memory, so without this a request the hardware cannot
// accept is re-attempted forever — and a fresh blah2 (container restart,
// crash loop) starts counting from zero and picks the same one straight
// back up. Observed on a live node: a retune that saturated the front end
// left the whole stack re-applying it several times a minute, and
// restarting blah2 did not clear it because this API kept serving it.
var retuneRejection = null;
// last RF/overload status blah2 posted, or null if none yet
var overloadStatus = null;

// blah2 polls this — plain CSV, matching the minimalism of /capture
app.get('/capture/retune', (req, res) => {
  // A rejected generation is reported as 0 ("nothing pending") rather than
  // withheld, so blah2's parser takes its existing no-op path unchanged.
  const generation = (retuneRejection
    && retuneRejection.generation === retune.generation) ? 0 : retune.generation;
  res.type('text/plain').send(
    generation + ',' + retune.fc + ','
    + retune.gainReductionA + ',' + retune.gainReductionB + ','
    + retune.lnaState
  );
});
// request a new candidate tuning (e.g. from retina-gui's Auto-Calibrate)
app.post('/capture/retune', express.json(), (req, res) => {
  const { fc, gainReductionA, gainReductionB, lnaState } = req.body || {};
  const error = retuneLib.validate(fc, gainReductionA, gainReductionB, lnaState);
  if (error) {
    return res.status(400).json({ success: false, error: error });
  }
  retune.generation += 1;
  retune.fc = fc;
  retune.gainReductionA = gainReductionA;
  retune.gainReductionB = gainReductionB;
  retune.lnaState = lnaState;
  // A new request supersedes any earlier give-up: this generation has not
  // been tried yet and deserves its own attempts, even if the previous one
  // was abandoned. (Retina-gui's Auto-Calibrate relies on this — after a
  // candidate is refused it retreats to a safer one, which must be allowed
  // through.)
  retuneRejection = null;
  // keep the ADS-B bistatic Doppler calc in sync with the radio's real fc
  config.capture.fc = fc;
  res.json({ success: true, generation: retune.generation });
});
// blah2 posts here after a retune is actually applied to the device
app.post('/capture/retune/ack', express.text(), (req, res) => {
  const parts = String(req.body).split(',').map(Number);
  if (parts.length === 6 && parts.every(Number.isFinite)) {
    retuneStatus = {
      generation: parts[0],
      fc: parts[1],
      gainReductionA: parts[2],
      gainReductionB: parts[3],
      lnaState: parts[4],
      appliedAt: parts[5],
      receivedAt: Date.now(),
    };
  }
  res.json({});
});
// blah2 posts here when it has exhausted its attempts at a generation and is
// giving up on it. Body: "<generation>,<attempts>". This is what stops the
// retry loop surviving a blah2 restart — see retuneRejection above.
app.post('/capture/retune/reject', express.text(), (req, res) => {
  const parts = String(req.body).split(',').map(Number);
  if (parts.length === 2 && parts.every(Number.isFinite)) {
    // Only the generation still pending can be rejected; a late rejection
    // for a superseded one must not suppress the newer request.
    if (parts[0] === retune.generation) {
      retuneRejection = {
        generation: parts[0],
        attempts: parts[1],
        rejectedAt: Date.now(),
      };
    }
  }
  res.json({});
});
// poll for the last applied retune. `rejected` is additive: callers that only
// read generation/appliedAt (retina-gui's Blah2Client) are unaffected, but it
// lets one distinguish "blah2 never saw it" from "the hardware refused it",
// which otherwise look identical from outside.
app.get('/capture/retune/status', (req, res) => {
  const body = Object.assign({}, retuneStatus || {});
  if (retuneRejection) {
    body.rejected = retuneRejection;
  }
  res.json(body);
});
// blah2 posts per-tuner RF overload state (on change + heartbeat). Deliberately
// its own endpoint, not /capture/rf-status — that path already belongs to the
// (unrelated) peak-dBFS meter feature; no consumer needs both in one call, so
// keeping them on separate endpoints avoids coupling two independent features
// together just because they both report "something about RF status."
// Accepts "overloadA,overloadB,timestamp" and the newer
// "overloadA,overloadB,timestamp,countA,countB". The counts are monotonic
// tallies of overload *onsets*, which a consumer needs because the flags are
// a level: this hardware clips and recovers faster than anyone polls, so an
// entire episode can pass with every sample of the level reading false. Both
// lengths are accepted so a newer API can run against an older blah2 without
// the endpoint silently discarding its posts.
app.post('/capture/overload-status', express.text(), (req, res) => {
  const parts = String(req.body).split(',').map(Number);
  if ((parts.length === 3 || parts.length === 5) && parts.every(Number.isFinite)) {
    overloadStatus = {
      overloadA: parts[0] === 1,
      overloadB: parts[1] === 1,
      timestamp: parts[2],
      receivedAt: Date.now(),
    };
    if (parts.length === 5) {
      overloadStatus.overloadCountA = parts[3];
      overloadStatus.overloadCountB = parts[4];
    }
  }
  res.json({});
});
// blah2 posts per-tuner peak dBFS here every ~1s — plain CSV, matching the
// minimalism of /capture
var rfStatus = null;
app.post('/capture/rf-status', express.text(), (req, res) => {
  const parts = String(req.body).split(',').map(Number);
  if (parts.length === 3 && parts.every(Number.isFinite)) {
    rfStatus = {
      peakDbfsA: parts[0],
      peakDbfsB: parts[1],
      timestamp: parts[2],
      receivedAt: Date.now(),
    };
  }
  res.json({});
});
// poll for RF overload state
app.get('/capture/overload-status', (req, res) => {
  res.json(overloadStatus || {});
});
// poll for the latest peak dBFS status
app.get('/capture/rf-status', (req, res) => {
  res.json(rfStatus || {});
});
app.listen(PORT, HOST, () => {
  console.log(`Running on http://${HOST}:${PORT}`);
});

// tcp listener map
const server_map = net.createServer((socket)=>{
    socket.on("data",(msg)=>{
        data_map = data_map + msg.toString();
        if (data_map.slice(-1) === "}")
        {
          map = data_map;
          data_map = '';
        }
    });
    socket.on("close",()=>{
        console.log("Connection closed.");
    })
});
server_map.listen(config.network.ports.map);

// tcp listener detection
let processingDetection = false;
const server_detection = net.createServer((socket)=>{
  socket.on("data", async (msg)=>{
      data_detection = data_detection + msg.toString();
      if (data_detection.slice(-1) === "}" && !processingDetection)
      {
        processingDetection = true;
        try {
          const det = JSON.parse(data_detection);
          if (adsbTruthAvailable()) {
            const aircraft = await getCachedAircraft();
            det.adsb = det.delay.map((delay, idx) => {
              const doppler = det.doppler[idx];
              let bestMatch = null;
              let bestScore = Infinity;
              for (const ac of aircraft) {
                if (!ac.lat || !ac.lon || (!ac.alt_geom && !ac.alt_baro)) continue;
                const expected_delay = bistatic.computeBistaticDelay(ac,
                  config.location.rx, config.location.tx);
                const expected_doppler = bistatic.computeBistaticDoppler(ac,
                  config.location.rx, config.location.tx, config.capture.fc);
                if (expected_delay === null || expected_doppler === null) continue;
                const delay_err = Math.abs(delay - expected_delay);
                const doppler_err = Math.abs(doppler - expected_doppler);
                const delay_tol = config.truth.adsb.delay_tolerance || 2.0;
                const doppler_tol = config.truth.adsb.doppler_tolerance || 5.0;
                if (delay_err < delay_tol && doppler_err < doppler_tol) {
                  const score = delay_err / delay_tol + doppler_err / doppler_tol;
                  if (score < bestScore) {
                    bestScore = score;
                    bestMatch = {
                      hex: ac.hex,
                      lat: ac.lat,
                      lon: ac.lon,
                      alt: ac.alt_geom ?? ac.alt_baro,
                      gs: ac.gs,
                      track: ac.track,
                      expected_delay: Math.round(expected_delay * 100) / 100,
                      expected_doppler: Math.round(expected_doppler * 100) / 100,
                      delay_residual: Math.round((delay - expected_delay) * 100) / 100,
                      doppler_residual: Math.round((doppler - expected_doppler) * 100) / 100
                    };
                  }
                }
              }
              return bestMatch;
            });
          }
          detection = JSON.stringify(det);
          // Forward to external tracker if enabled
          forwardToTracker(detection);
        } catch (e) {
          console.error('Detection processing error:', e.message);
          detection = data_detection;
        } finally {
          data_detection = '';
          processingDetection = false;
        }
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
server_detection.listen(config.network.ports.detection);

// tcp listener tracker
const server_tracker = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
      data_tracker = data_tracker + msg.toString();
      if (data_tracker.slice(-1) === "}")
      {
        track = data_tracker;
        data_tracker = '';
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
server_tracker.listen(config.network.ports.track);

// tcp listener timestamp
const server_timestamp = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_timestamp = data_timestamp + msg.toString();
    timestamp = data_timestamp;
    data_timestamp = '';
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
server_timestamp.listen(config.network.ports.timestamp);

// tcp listener timing
const server_timing = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_timing = data_timing + msg.toString();
    if (data_timing.slice(-1) === "}")
    {
      timing = data_timing;
      data_timing = '';
    }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
server_timing.listen(config.network.ports.timing);

// tcp listener iqdata metadata
const server_iqdata = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_iqdata = data_iqdata + msg.toString();
    if (data_iqdata.slice(-1) === "}")
    {
      iqdata = data_iqdata;
      data_iqdata = '';
    }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
server_iqdata.listen(config.network.ports.iqdata);

let aircraftCache = [];
let lastFetchTime = 0;
const CACHE_INTERVAL = 1000;
const HTTP_TIMEOUT = 5000;

async function fetchADSB() {
  if (!config.truth.adsb.enabled) {
    return [];
  }
  const tar1090_url = `http://${config.truth.adsb.tar1090}/data/aircraft.json`;
  return new Promise((resolve) => {
    const req = http.get(tar1090_url, { timeout: HTTP_TIMEOUT }, (resp) => {
      let data = '';
      resp.on('data', (chunk) => { data += chunk; });
      resp.on('end', () => {
        try {
          const json = JSON.parse(data);
          resolve(json.aircraft || []);
        } catch (e) {
          console.error('Error parsing tar1090 response:', e.message);
          resolve([]);
        }
      });
    }).on('error', (err) => {
      console.error('Error fetching from tar1090:', err.message);
      resolve([]);
    });
    req.on('timeout', () => {
      req.destroy();
      console.error('tar1090 request timeout after', HTTP_TIMEOUT, 'ms');
      resolve([]);
    });
  });
}

async function getCachedAircraft() {
  const now = Date.now();
  if (now - lastFetchTime > CACHE_INTERVAL) {
    aircraftCache = await fetchADSB();
    lastFetchTime = now;
  }
  return aircraftCache;
}

process.on('SIGTERM', () => {
  console.log('SIGTERM signal received.');
  process.exit(0);
});

function buildAdsbQuery(api_url, config) {
  const rx_str = `${config.location.rx.latitude},${config.location.rx.longitude},${config.location.rx.altitude}`;
  const tx_str = `${config.location.tx.latitude},${config.location.tx.longitude},${config.location.tx.altitude}`;
  const fc_mhz = Math.round(config.capture.fc / 1000000);
  const server_url = `http://${config.truth.adsb.tar1090}`;

  const params = new URLSearchParams({
    server: server_url,
    rx: rx_str,
    tx: tx_str,
    fc: fc_mhz
  });

  return `${api_url}?${params.toString()}`;
}

async function fetchJson(url) {
  return new Promise((resolve) => {
    const req = http.get(url, { timeout: HTTP_TIMEOUT }, (resp) => {
      let data = '';
      resp.on('data', (chunk) => { data += chunk; });
      resp.on('end', () => {
        try {
          resolve(JSON.parse(data));
        } catch (e) {
          console.error('Error parsing response:', e.message);
          resolve({});
        }
      });
    }).on('error', (err) => {
      console.error('HTTP request error:', err.message);
      resolve({});
    });

    req.on('timeout', () => {
      req.destroy();
      console.error('Request timeout after', HTTP_TIMEOUT, 'ms');
      resolve({});
    });
  });
}

async function fetchFromAdsbService() {
  const api_url = "http://" + config.truth.adsb.adsb2dd + "/api/dd";
  const api_query = buildAdsbQuery(api_url, config);
  const response = await fetchJson(api_query);
  return response;
}

function compareAdsbResults(legacyData, newData) {
  const legacyHexes = new Set(Object.keys(legacyData));
  const newHexes = new Set(Object.keys(newData));
  
  const bothHexes = [...legacyHexes].filter(hex => newHexes.has(hex));
  
  let totalDelayDiff = 0;
  let totalDopplerDiff = 0;
  let delayCount = 0;
  let dopplerCount = 0;
  
  const discrepancies = [];
  
  for (const hex of bothHexes) {
    const legacy = legacyData[hex];
    const newAc = newData[hex];
    
    const legacyDelay = parseFloat(legacy.delay);
    const newDelay = parseFloat(newAc.delay);
    const legacyDoppler = legacy.doppler ? parseFloat(legacy.doppler) : null;
    const newDoppler = newAc.doppler || null;
    
    if (!isNaN(legacyDelay) && !isNaN(newDelay)) {
      const delayDiff = Math.abs(newDelay - legacyDelay);
      totalDelayDiff += delayDiff;
      delayCount++;
      
      let dopplerDiff = null;
      if (legacyDoppler !== null && newDoppler !== null && !isNaN(legacyDoppler) && !isNaN(newDoppler)) {
        dopplerDiff = Math.abs(newDoppler - legacyDoppler);
        totalDopplerDiff += dopplerDiff;
        dopplerCount++;
      }
      
      discrepancies.push({
        hex: hex,
        flight: newAc.flight || legacy.flight || '',
        delay_legacy: Math.round(legacyDelay * 100) / 100,
        delay_new: Math.round(newDelay * 100) / 100,
        delay_diff: Math.round(delayDiff * 100) / 100,
        doppler_legacy: legacyDoppler !== null ? Math.round(legacyDoppler * 100) / 100 : null,
        doppler_new: newDoppler !== null ? Math.round(newDoppler * 100) / 100 : null,
        doppler_diff: dopplerDiff !== null ? Math.round(dopplerDiff * 100) / 100 : null
      });
    }
  }
  
  discrepancies.sort((a, b) => b.delay_diff - a.delay_diff);
  
  return {
    total_aircraft: {
      legacy: legacyHexes.size,
      new: newHexes.size,
      both: bothHexes.length,
      legacy_only: legacyHexes.size - bothHexes.length,
      new_only: newHexes.size - bothHexes.length
    },
    avg_delay_diff: delayCount > 0 ? Math.round((totalDelayDiff / delayCount) * 100) / 100 : null,
    avg_doppler_diff: dopplerCount > 0 ? Math.round((totalDopplerDiff / dopplerCount) * 100) / 100 : null,
    largest_discrepancies: discrepancies.slice(0, 10)
  };
}

async function fetchFromTar1090AndExtrapolate(clientDetectionTs) {
  const aircraft = await getCachedAircraft();

  let detectionTimestamp;
  if (clientDetectionTs) {
    detectionTimestamp = clientDetectionTs;
  } else {
    detectionTimestamp = Date.now() / 1000;
    try {
      if (detection) {
        const detectionData = JSON.parse(detection);
        if (detectionData.timestamp) {
          detectionTimestamp = detectionData.timestamp / 1000;
        }
      }
    } catch (e) {
      console.error('Error parsing detection timestamp:', e.message);
    }
  }

  const adsbData = {};
  for (const ac of aircraft) {
    if (!ac.hex) continue;

    const timestamp = Date.now() / 1000 - (ac.seen_pos || 0);

    adsbData[ac.hex] = {
      hex: ac.hex,
      flight: ac.flight || '',
      timestamp: timestamp,
      lat: ac.lat,
      lon: ac.lon,
      alt_geom: ac.alt_geom,
      alt_baro: ac.alt_baro,
      gs: ac.gs,
      track: ac.track,
      geom_rate: ac.geom_rate
    };
  }

  const rxPos = {
    lat: config.location.rx.latitude,
    lon: config.location.rx.longitude,
    alt: config.location.rx.altitude
  };
  const txPos = {
    lat: config.location.tx.latitude,
    lon: config.location.tx.longitude,
    alt: config.location.tx.altitude
  };

  const synchronized = extrapolateAdsbData(
    adsbData,
    detectionTimestamp,
    rxPos,
    txPos,
    config.capture.fc
  );

  return synchronized;
}

app.get('/api/adsb2dd', async (req, res) => {
  if (!adsbTruthAvailable()) {
    return res.status(400).end();
  }

  try {
    let result = {};

    const clientTs = req.query.detection_ts ? parseFloat(req.query.detection_ts) / 1000 : undefined;

    if (config.truth.adsb.use_legacy_method) {
      result = await fetchFromAdsbService();
    } else {
      result = await fetchFromTar1090AndExtrapolate(clientTs);
    }

    res.json(result);
  } catch (error) {
    console.error('Error in /api/adsb2dd:', error);
    res.json({});
  }
});

app.get('/api/adsb2dd/diagnostic', async (req, res) => {
  if (!adsbTruthAvailable()) {
    return res.status(400).end();
  }

  try {
    const clientTs = req.query.detection_ts ? parseFloat(req.query.detection_ts) / 1000 : undefined;
    const legacyResult = await fetchFromAdsbService();
    const newResult = await fetchFromTar1090AndExtrapolate(clientTs);

    const comparison = compareAdsbResults(legacyResult, newResult);

    const result = {
      method: 'diagnostic',
      legacy: legacyResult,
      new: newResult,
      comparison: comparison
    };

    res.json(result);
  } catch (error) {
    console.error('Error in /api/adsb2dd/diagnostic:', error);
    res.json({});
  }
});
