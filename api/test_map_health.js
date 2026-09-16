// Exercise the actual route with Express; no SDR or external service required.
const fs = require('fs');
const vm = require('vm');
const http = require('http');
const assert = require('assert/strict');
const express = require('express');
const source = fs.readFileSync(__dirname+'/server.js','utf8');
const route = source.match(/app\.get\('\/api\/map-health', \(req, res\) => \{[\s\S]*?\n\}\);/);
assert(route && route.length === 1);
const app = express();
const context = {app,map:''};
vm.runInNewContext(route[0],context);
app.get('/api/map',(_req,res)=>res.send(context.map));
const get = (port,path) => new Promise((resolve,reject)=>{
  http.get({host:'127.0.0.1',port,path},res=>{
    const chunks=[];res.on('data',x=>chunks.push(x));
    res.on('end',()=>resolve({status:res.statusCode,type:res.headers['content-type'],body:Buffer.concat(chunks)}));
  }).on('error',reject);
});
(async()=>{
  const server=app.listen(0,'127.0.0.1');
  await new Promise(resolve=>server.once('listening',resolve));
  const port=server.address().port;
  try {
    let tests=0;
    for (const map of ['', '{"timestamp":2000000000000,"data":[1,2]}',
        '{"timestamp":1999999900000,"data":[]}',
        JSON.stringify({timestamp:2000000000000,data:Array.from({length:301},()=>Array(411).fill(-12.34))})]) {
      context.map=map;
      const full=await get(port,'/api/map');
      const health=await get(port,'/api/map-health');
      assert.equal(health.status,200);
      assert.match(health.type,/^text\/plain/);
      assert.deepEqual(full.body,Buffer.from(map));
      assert.deepEqual(health.body,full.body.subarray(0,23));
      tests++;
    }
    // Freshness remains tied to the map even while unrelated state advances.
    context.timestamp=2000000010000;
    const before=await get(port,'/api/map-health');
    context.timestamp=2000000020000;
    assert.deepEqual((await get(port,'/api/map-health')).body,before.body);
    console.log(JSON.stringify({pass:true,mapCases:tests,independentHeartbeatDoesNotRefreshMap:true,
      node:process.version,express:require('express/package.json').version}));
  } finally {await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);process.exitCode=1;});
