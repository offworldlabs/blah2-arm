const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert/strict');

function load(name) {
  let receive;
  const exported = { exports: {} };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'stash', name + '.js'), 'utf8'), {
    module: exported,
    console,
    require(name) {
      assert.equal(name, './updates.js');
      return { subscribe(_kind, fn) { receive = fn; } };
    }
  });
  return {
    push(value) { receive(JSON.stringify(value)); },
    get() { return JSON.parse(JSON.stringify(Object.values(exported.exports)[0]())); }
  };
}

const maxhold = load('maxhold');
const iq = load('iqdata');
const timing = load('timing');
const detection = load('detection');
const maps = [];
for (let i = 0; i < 45; ++i) {
  const map = { timestamp: i * 500, data: [[i, i === 0 ? 0 : -i], [i % 7, -2]], delay: [0, 1], doppler: [0, 1] };
  maps.push(map.data);
  if (maps.length > 20) maps.shift();
  maxhold.push(map);
  const expected = [0, 1].map(r => [0, 1].map(c => Math.max(...maps.map(m => m[r][c]))));
  assert.deepEqual(maxhold.get(), { ...map, data: expected });
  maxhold.push(map);
  assert.deepEqual(maxhold.get(), { ...map, data: expected });
  iq.push({ timestamp: i * 500, spectrum: [i], frequency: [10] });
  timing.push({ nCpi: i, cpi: i, uptime: 0, old_stage: 1 });
}
assert.deepEqual(iq.get().spectrum, Array.from({ length: 20 }, (_, j) => [25 + j]));
assert.deepEqual(iq.get().timestamp, Array.from({ length: 20 }, (_, j) => (25 + j) * 500));
assert.deepEqual(timing.get().cpi, Array.from({ length: 20 }, (_, j) => 25 + j));
assert.equal(timing.get().nCpi, undefined);
assert.equal(timing.get().uptime, undefined);
timing.push({ nCpi: 45, cpi: 45, new_stage: 4 });
assert.equal(timing.get().old_stage, undefined);
assert.deepEqual(timing.get().new_stage, [4]);
timing.push({ nCpi: 45, cpi: 45, new_stage: 4 });
assert.deepEqual(timing.get().new_stage, [4]);
timing.push({ nCpi: 0, cpi: 10, old_stage: 3 });
assert.deepEqual(timing.get().old_stage, [3]);
assert.equal(timing.get().new_stage, undefined);

const first = { timestamp: 500, delay: [1, 2], doppler: [3, 4], snr: [5, 6] };
detection.push(first);
detection.push(first);
assert.deepEqual(detection.get(), { timestamp: [500, 500], delay: [1, 2], doppler: [3, 4], snr: [5, 6] });
detection.push({ timestamp: 301000, delay: [7], doppler: [8], snr: [9] });
assert.deepEqual(detection.get(), { timestamp: [301000], delay: [7], doppler: [8], snr: [9] });
console.log(JSON.stringify({ pass: true, maxholdFrames: 45, window: 20, duplicates: true, staleTimingKeys: true, detectionExpiry: true }));
