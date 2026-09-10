// Test for network.tracker_forward: the path that feeds detection frames to
// the retina-tracker sidecar.
//
// Boots server.js as a child process against a stand-in tracker listening on a
// high port, pushes two detection frames into the detection TCP port the way
// blah2 does, and checks what actually arrives on the other side.
//
// The case worth having: retina-tracker frames this socket by splitting on
// "\n". server.js used to write the JSON with no delimiter, so two frames
// arrived as one unparseable run of text and the far end silently never saw a
// detection. testTwoFramesArriveSeparately is what pins that.
//
// Run with: node test_tracker_forward.js

const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

// must be 3000: the stash modules self-poll the API on a hardcoded port 3000
// and crash the process (no error handler) if nothing is listening there
const API_PORT = 3000;
const DETECTION_PORT = 33012;
const TRACKER_PORT = 33013;

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

function frame(timestamp) {
  return {
    timestamp,
    delay: [12.34, 88.01],
    doppler: [-45.6, 12.0],
    snr: [11.2, 4.8],
  };
}

// Stand-in for retina-tracker: accepts one connection and frames its input the
// same way retina_tracker/server.py::run_tcp_server does.
function startFakeTracker() {
  const received = [];
  let buffer = '';
  let connected = false;
  const server = net.createServer((socket) => {
    connected = true;
    socket.on('data', (chunk) => {
      buffer += chunk.toString();
      let index;
      while ((index = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, index).trim();
        buffer = buffer.slice(index + 1);
        if (line) received.push(line);
      }
    });
  });
  return new Promise((resolve) => {
    server.listen(TRACKER_PORT, '127.0.0.1', () => {
      resolve({ server, received, pending: () => buffer, isConnected: () => connected });
    });
  });
}

function sendDetection(obj) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(DETECTION_PORT, '127.0.0.1', () => {
      socket.write(JSON.stringify(obj), () => {
        // server.js processes once the accumulated buffer ends in "}", then
        // clears it; closing here is what makes each frame a discrete write.
        socket.end();
        resolve();
      });
    });
    socket.on('error', reject);
  });
}

function waitForServer(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const poll = () => {
      const req = http.get(`http://127.0.0.1:${API_PORT}/api/detection`, (res) => {
        res.resume();
        resolve();
      });
      req.on('error', () => {
        if (Date.now() > deadline) reject(new Error('server.js did not come up in time'));
        else setTimeout(poll, 200);
      });
    };
    poll();
  });
}

function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve) => {
    const poll = () => {
      if (predicate() || Date.now() > deadline) resolve(predicate());
      else setTimeout(poll, 50);
    };
    poll();
  });
}

async function main() {
  const cfg = `
network:
  ip: '127.0.0.1'
  ports:
    api: ${API_PORT}
    map: 33010
    detection: ${DETECTION_PORT}
    track: 33011
    timestamp: 34010
    timing: 34011
    iqdata: 34012
  tracker_forward:
    enabled: true
    host: '127.0.0.1'
    port: ${TRACKER_PORT}
capture:
  fc: 98000000
  device:
    gainReduction: [40, 41]
    lnaState: 4
truth:
  adsb:
    enabled: false
    tar1090: ""
    adsb2dd: ""
location:
  rx: {latitude: 0, longitude: 0, altitude: 0}
  tx: {latitude: 0, longitude: 0, altitude: 0}
`;
  const cfgPath = path.join(os.tmpdir(), `test_tracker_forward_config_${process.pid}.yml`);
  fs.writeFileSync(cfgPath, cfg);

  // The tracker has to be listening before server.js starts: connectToTracker()
  // runs at module load, and a refused connection only retries after 5s.
  const tracker = await startFakeTracker();

  const server = spawn('node', ['server.js', cfgPath],
    { cwd: __dirname, stdio: ['ignore', 'pipe', 'pipe'] });
  server.stderr.on('data', (d) => process.stderr.write(`[server] ${d}`));

  try {
    await waitForServer(10000);

    // forwardToTracker() drops silently while trackerConnected is false, and
    // the connect callback can land after the API is already serving. Waiting
    // on the far end's accept is what makes this deterministic rather than a
    // race the first frame usually loses.
    await waitFor(tracker.isConnected, 5000);

    console.log('tracker_forward:');
    check('server.js connects to the tracker on startup', tracker.isConnected());

    await sendDetection(frame(1757400000000));
    await waitFor(() => tracker.received.length >= 1, 5000);
    check('a detection frame reaches the tracker', tracker.received.length >= 1,
      `received ${tracker.received.length}`);

    if (tracker.received.length >= 1) {
      let parsed = null;
      try { parsed = JSON.parse(tracker.received[0]); } catch (e) { /* reported below */ }
      check('what arrives is one parseable frame', parsed !== null,
        tracker.received[0] && tracker.received[0].slice(0, 60));
      check('the payload survives the hop intact',
        parsed && parsed.timestamp === 1757400000000
          && JSON.stringify(parsed.delay) === JSON.stringify([12.34, 88.01])
          && JSON.stringify(parsed.snr) === JSON.stringify([11.2, 4.8]));
    }

    // The regression. Two frames must arrive as two delimited lines; with no
    // newline they concatenate into "}{" and the far end parses neither.
    await sendDetection(frame(1757400001000));
    await waitFor(() => tracker.received.length >= 2, 5000);
    check('two frames arrive as two separate lines', tracker.received.length === 2,
      `received ${tracker.received.length}`);
    check('nothing is left stuck in the frame buffer', tracker.pending() === '',
      JSON.stringify(tracker.pending()).slice(0, 60));

    if (tracker.received.length >= 2) {
      const second = JSON.parse(tracker.received[1]);
      check('the second frame is the second frame', second.timestamp === 1757400001000);
    }
  } finally {
    server.kill();
    tracker.server.close();
    fs.unlinkSync(cfgPath);
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
