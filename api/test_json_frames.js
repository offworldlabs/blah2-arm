const assert = require('assert/strict');
const jsonFrames = require('./json-frames');

const objects = [{ text: 'braces } {, quote ", slash \\, emoji 🙂', nested: [{ a: 2 }] }, { b: 3 }];
const expected = objects.map(JSON.stringify);
const wire = Buffer.from(' \n' + expected.join(' \t') + '\n');
let cases = 0;
for (let width = 1; width <= wire.length; ++width) {
  const got = [];
  const feed = jsonFrames(body => got.push(body));
  for (let start = 0; start < wire.length; start += width) {
    feed(wire.subarray(start, start + width));
  }
  assert.deepEqual(got, expected);
  ++cases;
}

// Abandoning an incomplete connection must not contaminate a new connection.
const got = [];
jsonFrames(body => got.push(body))(Buffer.from('{"unfinished":'));
jsonFrames(body => got.push(body))(Buffer.from(expected[1]));
assert.deepEqual(got, [expected[1]]);

const empty = jsonFrames(() => assert.fail('Whitespace emitted a frame'));
empty(Buffer.from(' \r\n\t'));
assert.throws(() => jsonFrames(() => {})('x'), /Expected radar JSON object/);
assert.throws(() => jsonFrames(() => {})('['), /Expected radar JSON object/);
assert.throws(() => jsonFrames(() => {})('{"x":"' + 'x'.repeat(16 * 1024 * 1024)), /exceeds limit/);
assert.throws(() => jsonFrames(() => { throw Error('sink failure'); })('{}'), /sink failure/);
// A rejected frame remains intact while the consumer is busy, then resumes once.
let ready = false;
const delivered = [];
const paused = jsonFrames(frame => {
  if (!ready) return false;
  delivered.push(frame);
});
assert.equal(paused(Buffer.from(expected.join(''))), false);
assert.equal(paused(Buffer.alloc(0)), false);
assert.deepEqual(delivered, []);
ready = true;
assert.equal(paused(Buffer.alloc(0)), true);
assert.deepEqual(delivered, expected);
assert.equal(paused(Buffer.alloc(0)), true);
assert.deepEqual(delivered, expected);
console.log(JSON.stringify({ pass: true, utf8FragmentationCases: cases, reconnectIsolation: true, bounds: true, backpressureRetry: true }));
