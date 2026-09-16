const updates = require('./updates.js');

var time = 300;
var map = [];
var timestamp = [];
var delay = [];
var doppler = [];
var detection = '';
var ts = '';
var output = [];
var lastUpdate;
updates.subscribe('detection', function(body) {
  try {
    const parsed = JSON.parse(body);
    const key = String(parsed.timestamp);
    if (lastUpdate === key) return;
    lastUpdate = key;
    ts = String(parsed.timestamp);

    detection = parsed;
    map.push(detection);
    for (let i = 0; i < map.length; i++)
    {
      if ((ts - map[i].timestamp)/1000 > time)
      {
        map.shift();
      }
      else
      {
        break;
      }
    }
    delay = [];
    doppler = [];
    timestamp = [];
    let snr = [];
    for (var i = 0; i < map.length; i++)
    {
      for (var j = 0; j < map[i].delay.length; j++)
      {
        delay.push(map[i].delay[j]);
        doppler.push(map[i].doppler[j]);
        snr.push(map[i].snr[j]);
        timestamp.push(map[i].timestamp);
      }
    }
    output = {
      timestamp: timestamp,
      delay: delay,
      doppler: doppler,
      snr: snr
    };

  } catch (e) {
    console.error(e.message);
  }
});

function get_data() {
  return output;
};

module.exports.get_data_detection = get_data;
