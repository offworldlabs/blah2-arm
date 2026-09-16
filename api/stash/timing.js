const updates = require('./updates.js');

var nCpi = 20;
var cpi = [];
var output = {};
var lastUpdate;
updates.subscribe('timing', function(body) {
  try {
    const parsed = JSON.parse(body);
    const key = String(parsed.nCpi);
    if (lastUpdate === key) return;
    lastUpdate = key;

    cpi = parsed;
    let keys = Object.keys(cpi);
    keys = keys.filter(item => item !== "uptime");
    keys = keys.filter(item => item !== "nCpi");
    // Preserve upstream behavior when a processor stops emitting a stage.
    for (const stale of Object.keys(output)) {
      if (!keys.includes(stale)) {
        delete output[stale];
      }
    }
    for (let i = 0; i < keys.length; i++) {
      if (!(keys[i] in output)) {
        output[keys[i]] = [];
      }
      output[keys[i]].push(cpi[keys[i]]);
      if (output[keys[i]].length > nCpi) {
        output[keys[i]].shift();
      }
    }

  } catch (e) {
    console.error(e.message);
  }
});

function get_data() {
  return output;
}

module.exports.get_data_timing = get_data;
