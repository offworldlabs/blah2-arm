const updates = require('./updates.js');

var nCpi = 20;
var map = [];
var maxhold = '';
// Traverse one row at a time while retaining the oldest-to-newest reduction.
function process(matrixArray) {
  const result = [];
  const rows = matrixArray[0].length;
  const cols = matrixArray[0][0].length;
  for (let i = 0; i < rows; ++i) {
    const row = matrixArray[0][i].slice(0, cols);
    for (let k = 1; k < matrixArray.length; ++k) {
      const source = matrixArray[k][i];
      for (let j = 0; j < cols; ++j) {
        row[j] = Math.max(row[j], source[j]);
      }
    }
    result.push(row);
  }
  return result;
}

var lastUpdate;
updates.subscribe('map', function(body) {
  try {
    const parsed = JSON.parse(body);
    const key = String(parsed.timestamp);
    if (lastUpdate === key) return;
    lastUpdate = key;

    maxhold = parsed;
    map.push(maxhold.data);
    if (map.length > nCpi) {
      map.shift();
    }
    maxhold.data = process(map);

  } catch (e) {
    console.error(e.message);
  }
});

function get_data() {
  return maxhold;
};

module.exports.get_data_map = get_data;
