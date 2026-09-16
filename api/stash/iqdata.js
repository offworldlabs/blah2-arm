const updates = require('./updates.js');

var nCpi = 20;
var spectrum = [];
var frequency = [];
var timestamp = [];
var output = [];
var lastUpdate;
updates.subscribe('iqdata', function(body) {
  try {
    const parsed = JSON.parse(body);
    const key = String(parsed.timestamp);
    if (lastUpdate === key) return;
    lastUpdate = key;

    output = parsed;
    // spectrum
    spectrum.push(output.spectrum);
    if (spectrum.length > nCpi) {
      spectrum.shift();
    }
    output.spectrum = spectrum;
    // frequency
    frequency.push(output.frequency);
    if (frequency.length > nCpi) {
      frequency.shift();
    }
    output.frequency = frequency;
    // timestamp
    timestamp.push(output.timestamp);
    if (timestamp.length > nCpi) {
      timestamp.shift();
    }
    output.timestamp = timestamp;

  } catch (e) {
    console.error(e.message);
  }
});

function get_data() {
  return output;
};

module.exports.get_data_iqdata = get_data;
