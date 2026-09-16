const { StringDecoder } = require('string_decoder');

// Each connection owns a decoder and parser. TCP chunks need not match objects.
module.exports = function jsonFrames(onFrame) {
  const decoder = new StringDecoder('utf8');
  let buffer = '';
  let start = -1;
  let scan = 0;
  let depth = 0;
  let quoted = false;
  let escaped = false;
  let pendingFrame = null;

  return function feed(chunk) {
    buffer += typeof chunk === 'string' ? chunk : decoder.write(chunk);
    // Bound an incomplete frame in UTF-16 code units (as used by JS strings).
    if (buffer.length + (pendingFrame?.length || 0) > 16 * 1024 * 1024) {
      throw Error('Radar frame exceeds limit');
    }
    // A false result means the consumer has not accepted this frame yet.
    if (pendingFrame !== null) {
      if (onFrame(pendingFrame) === false) return false;
      pendingFrame = null;
    }
    for (; scan < buffer.length; ++scan) {
      const c = buffer[scan];
      if (start < 0) {
        if (/\s/.test(c)) continue;
        if (c !== '{') throw Error('Expected radar JSON object');
        start = scan;
        depth = 1;
        continue;
      }
      if (quoted) {
        if (escaped) escaped = false;
        else if (c === '\\') escaped = true;
        else if (c === '"') quoted = false;
        continue;
      }
      if (c === '"') quoted = true;
      else if (c === '{' || c === '[') ++depth;
      else if (c === '}' || c === ']') {
        if (--depth === 0) {
          const frame = buffer.slice(start, scan + 1);
          buffer = buffer.slice(scan + 1);
          scan = -1;
          start = -1;
          if (onFrame(frame) === false) {
            pendingFrame = frame;
            scan = 0;
            return false;
          }
        }
      }
    }
    if (start < 0) {
      buffer = '';
      scan = 0;
    }
    return true;
  };
};
