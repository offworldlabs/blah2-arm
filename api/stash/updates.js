// Shared in-process notifications. Each stash owns its parsed payload.
const handlers = new Map();

exports.subscribe = (kind, fn) => {
  if (handlers.has(kind)) throw Error('Duplicate stash');
  handlers.set(kind, fn);
};

exports.publish = (kind, body) => {
  const fn = handlers.get(kind);
  if (fn) fn(body);
};
