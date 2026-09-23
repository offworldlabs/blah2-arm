// Generates openapi.json from the Express routes in server.js.
// Run with: npm run openapi (write) or npm run openapi:check (CI, exits 1 if stale)

const fs = require('fs');
const path = require('path');
const swaggerAutogen = require('swagger-autogen');
const pkg = require('./package.json');

const SOURCE = path.join(__dirname, 'server.js');
const OUTPUT = path.join(__dirname, 'openapi.json');

// network.ports.api in config/config.yml; the stash modules and host/nginx.conf
// also assume 3000
const PORT = 3000;

// keyed by first path segment; '' is the root route
const TAGS = [
  {
    segment: 'api',
    name: 'api',
    description: 'Latest radar output relayed from blah2 over TCP (map, detections, '
      + 'timestamp, timing, IQ metadata), the ADS-B truth flag, and ADS-B '
      + 'aircraft projected into bistatic delay-Doppler.',
  },
  {
    segment: 'stash',
    name: 'stash',
    description: 'Short histories built by polling /api on each new timestamp: the '
      + 'map max-held over the last 20 CPIs, detections from the last 300 s, and '
      + 'spectrum and timing for the last 20 CPIs.',
  },
  {
    segment: 'capture',
    name: 'capture',
    description: 'Control channel between the blah2 core and this API. blah2 polls '
      + 'the IQ recording flag and pending retune, and posts retune acks and '
      + 'rejections, RF overload and peak dBFS; clients request a retune with '
      + 'POST /capture/retune and read the retune, overload and RF status back.',
  },
  {
    segment: '',
    name: 'health',
    description: 'Answers whenever the Express server is up, whatever the state of blah2.',
  },
];

function tagFor(route) {
  const segment = route.split('/')[1];
  const tag = TAGS.find((t) => t.segment === segment);
  if (!tag) {
    throw new Error(`no tag for /${segment} routes: add one to TAGS in openapi.js`);
  }
  return tag.name;
}

async function generate() {
  // writeOutputFile is off, so the output path is required but never written
  const result = await swaggerAutogen({
    openapi: '3.0.0',
    disableLogs: true,
    writeOutputFile: false,
    autoHeaders: false,
    autoQuery: true,
    autoBody: true,
    autoResponse: true,
  })(OUTPUT, [SOURCE]);
  if (!result || !result.success) {
    throw new Error('swagger-autogen failed to parse server.js');
  }

  // info, servers and tags are replaced below
  const { openapi, info, servers, tags, paths, ...rest } = result.data;
  const taggedPaths = {};
  for (const [route, operations] of Object.entries(paths)) {
    taggedPaths[route] = {};
    for (const [method, operation] of Object.entries(operations)) {
      // tags set in a #swagger comment take precedence
      taggedPaths[route][method] = {
        ...operation,
        tags: operation.tags?.length ? operation.tags : [tagFor(route)],
      };
    }
  }

  const spec = {
    openapi,
    info: {
      title: 'blah2 API',
      version: pkg.version,
      description: 'HTTP API of blah2-api, which runs on each RETINA node. '
        + 'Generated from api/server.js by api/openapi.js.',
    },
    servers: [
      {
        url: `http://{node}:${PORT}`,
        description: 'blah2-api on the node, listening on all interfaces (host networking)',
        variables: {
          node: {
            default: 'localhost',
            description: 'localhost on the node itself, or the node\'s mDNS name '
              + '(e.g. ret4c844c20.local) from the same network',
          },
        },
      },
    ],
    tags: TAGS.map(({ name, description }) => ({ name, description })),
    paths: taggedPaths,
    ...rest,
  };
  return JSON.stringify(spec, null, 2) + '\n';
}

async function main() {
  const check = process.argv.includes('--check');
  const generated = await generate();
  if (!check) {
    fs.writeFileSync(OUTPUT, generated);
    console.log('Wrote api/openapi.json');
    return;
  }

  const committed = fs.existsSync(OUTPUT) ? fs.readFileSync(OUTPUT, 'utf8') : null;
  if (committed !== generated) {
    console.error('api/openapi.json is out of date with server.js or openapi.js.');
    console.error('Regenerate it with `npm run openapi` in api/ and commit the result.');
    process.exit(1);
  }
  console.log('api/openapi.json is up to date.');
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
