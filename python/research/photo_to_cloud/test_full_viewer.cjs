// Tests the exact offline worker embedded in the full viewer; no browser or network is required.
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, 'full_viewer.html'), 'utf8');
const match = html.match(/<script id="parser-worker" type="text\/x-worker">([\s\S]*?)<\/script>/);
assert(match, 'PLY worker missing');
async function runWorker(buffer, factor, options={}) {
  const messages = [];
  const context = {self: {postMessage: value => messages.push(value)}, TextDecoder, TextEncoder, Blob, Uint8Array,
                   Float32Array, Float64Array, DataView, Number, Math, Object, Error, String, Infinity};
  vm.createContext(context);
  vm.runInContext(match[1], context);
  const file = {arrayBuffer: async () => buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength)};
  await context.self.onmessage({data: factor === undefined ? file : {kind: 'export', file, factor, ...options}});
  return messages[messages.length - 1];
}
const parse = buffer => runWorker(buffer);
function header(format, properties, count=2) {
  return Buffer.from(`ply\nformat ${format} 1.0\nelement vertex ${count}\n${properties}\nend_header\n`);
}
(async () => {
  const properties = 'property float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue';
  const binary = Buffer.alloc(30);
  for (let i=0; i<2; i++) {const p=i*15; binary.writeFloatLE(i+1,p); binary.writeFloatLE(i+2,p+4); binary.writeFloatLE(i+3,p+8); binary[p+12]=i?20:100; binary[p+13]=50; binary[p+14]=200;}
  let result = await parse(Buffer.concat([header('binary_little_endian', properties), binary]));
  assert.equal(result.type, 'done'); assert.equal(result.valid, 2);
  assert.deepEqual(Array.from(new Uint8Array(result.colors)), [100,50,200,20,50,200]);
  assert.deepEqual(Array.from(new Float32Array(result.positions)), [-.5,-.5,-.5,.5,.5,.5]);
  const listProperties = 'property float x\nproperty float y\nproperty float z\nproperty list uchar int views\nproperty uchar red\nproperty uchar green\nproperty uchar blue';
  const big = Buffer.alloc(48);
  for (let i=0; i<2; i++) {const p=i*24; big.writeFloatBE(i+1,p); big.writeFloatBE(i+2,p+4); big.writeFloatBE(i+3,p+8); big[p+12]=2; big.writeInt32BE(7,p+13); big.writeInt32BE(8,p+17); big[p+21]=10+i; big[p+22]=20; big[p+23]=30;}
  result = await parse(Buffer.concat([header('binary_big_endian', listProperties), big]));
  assert.equal(result.type, 'done'); assert.deepEqual(Array.from(new Uint8Array(result.colors)), [10,20,30,11,20,30]);
  result = await parse(Buffer.concat([header('ascii', listProperties, 3), Buffer.from('1 2 3 2 7 8 10 20 30\n4 5 6 0 90 80 70\nNaN 0 0 0 1 2 3\n')]));
  assert.equal(result.type, 'done'); assert.equal(result.valid, 2); assert.equal(result.total, 3);
  result = await parse(Buffer.concat([header('binary_little_endian', properties), binary.subarray(0, 20)]));
  assert.equal(result.type, 'error');
  const binaryFile = Buffer.concat([header('binary_little_endian', properties), binary, Buffer.from([7, 8, 9])]);
  result = await runWorker(binaryFile, 2);
  assert.equal(result.type, 'exported');
  let out = Buffer.from(await result.blob.arrayBuffer()), off = out.indexOf('end_header\n') + 'end_header\n'.length;
  assert.equal(out.readFloatLE(off), 2); assert.equal(out.readFloatLE(off + 15), 4);
  assert.deepEqual(out.subarray(off + 12, off + 15), binary.subarray(12, 15));
  assert.deepEqual(out.subarray(-3), Buffer.from([7, 8, 9]));
  assert(out.subarray(0, off).includes('comment SpatialPhotoCloud units meters'));
  const reopened = await parse(out); assert.equal(reopened.type, 'done'); assert.equal(reopened.metric, true);
  const bigFile = Buffer.concat([header('binary_big_endian', listProperties), big]);
  result = await runWorker(bigFile, 0.5); assert.equal(result.type, 'exported');
  out = Buffer.from(await result.blob.arrayBuffer()); off = out.indexOf('end_header\n') + 'end_header\n'.length;
  assert.equal(out.readFloatBE(off), 0.5); assert.equal(out.readFloatBE(off + 24), 1);
  assert.deepEqual(out.subarray(off + 12, off + 24), big.subarray(12, 24));
  const ascii = Buffer.concat([header('ascii', listProperties), Buffer.from('1 2 3 2 7 8 10 20 30\n4 5 6 0 90 80 70\n3 0 1 2\n')]);
  result = await runWorker(ascii, 2); assert.equal(result.type, 'exported');
  out = Buffer.from(await result.blob.arrayBuffer()).toString();
  assert(out.endsWith('2 4 6 2 7 8 10 20 30\n8 10 12 0 90 80 70\n3 0 1 2\n'));
  const integers = Buffer.from('ply\nformat ascii 1.0\nelement vertex 1\nproperty int x\nproperty int y\nproperty int z\nend_header\n1 2 3\n');
  result = await runWorker(integers, 2); assert.equal(result.type, 'error');
  result = await runWorker(binaryFile, Infinity); assert.equal(result.type, 'error');
  result = await runWorker(binaryFile, 1e100); assert.equal(result.type, 'error');
  const normalProps = 'property float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\nproperty uchar red';
  const normalData = Buffer.alloc(25);
  [2,1,1,1,0,0].forEach((v,i)=>normalData.writeFloatLE(v,i*4));normalData[24]=123;
  const withNormal = Buffer.concat([header('binary_little_endian', normalProps, 1),normalData,Buffer.from([42])]);
  result = await runWorker(withNormal, 2, {angles:[0,0,Math.PI/2],center:[1,1,1],up:'y',metric:true});
  assert.equal(result.type, 'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  const approx=(actual,expected)=>assert(Math.abs(actual-expected)<1e-6,`${actual} != ${expected}`);
  [2,4,2,0,1,0].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));assert.equal(out[off+24],123);assert.equal(out[out.length-1],42);
  result = await runWorker(withNormal, 2, {angles:[0,0,Math.PI/2],center:[1,1,1],up:'z',metric:true});
  assert.equal(result.type, 'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [2,-2,4,0,0,1].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));
  const reopenedMetricZ=await parse(out);assert.equal(reopenedMetricZ.metric,true);assert.equal(reopenedMetricZ.up,'z');
  result = await runWorker(withNormal, 1, {angles:[0,0,0],center:[0,0,0],up:'z',metric:false});
  assert.equal(result.type, 'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [2,-1,1,1,0,0].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));assert(!out.subarray(0,off).includes('units meters'));
  const reopenedZ=await parse(out);assert.equal(reopenedZ.metric,false);assert.equal(reopenedZ.up,'z');
  result=await runWorker(out,1,{angles:[-Math.PI/2,0,0],center:[2,-1,1],up:'z',metric:false,inputUp:'z'});
  assert.equal(result.type,'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [2,-1,1,1,0,0].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));
  result=await runWorker(withNormal,1,{center:[1,1,1],mirror:[1,0,0],metric:false});
  assert.equal(result.type,'exported');let mirrored=Buffer.from(await result.blob.arrayBuffer());off=mirrored.indexOf('end_header\n')+'end_header\n'.length;
  [0,1,1,-1,0,0].forEach((v,i)=>approx(mirrored.readFloatLE(off+i*4),v));assert.equal(mirrored[off+24],123);assert.equal(mirrored[mirrored.length-1],42);
  result=await runWorker(mirrored,1,{center:[1,1,1],mirror:[1,0,0],metric:false});
  assert.equal(result.type,'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [2,1,1,1,0,0].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));
  result=await runWorker(withNormal,1,{center:[1,1,1],mirror:[1,0,1],metric:false});
  assert.equal(result.type,'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [1,1,0,0,0,-1].forEach((v,i)=>approx(out.readFloatLE(off+i*4),v));
  // Independent matrix oracle for reflection + XYZ rotation + scaling + Z-up.
  const angles=[.2,-.3,.4],n=[.6,.8,0],v=[1,0,0],dot=v.reduce((s,x,i)=>s+x*n[i],0);
  const reflected=v.map((x,i)=>x-2*dot*n[i]);
  const multiply=(matrix,vec)=>matrix.map(row=>row.reduce((s,x,i)=>s+x*vec[i],0));
  const [ax,ay,az]=angles,rotX=[[1,0,0],[0,Math.cos(ax),-Math.sin(ax)],[0,Math.sin(ax),Math.cos(ax)]],rotY=[[Math.cos(ay),0,Math.sin(ay)],[0,1,0],[-Math.sin(ay),0,Math.cos(ay)]],rotZ=[[Math.cos(az),-Math.sin(az),0],[Math.sin(az),Math.cos(az),0],[0,0,1]],toZ=[[1,0,0],[0,0,-1],[0,1,0]];
  const expectedNormal=[rotX,rotY,rotZ,toZ].reduce((vec,matrix)=>multiply(matrix,vec),reflected),origin=[1,-1,1],expectedPoint=expectedNormal.map((x,i)=>2*(x+origin[i]));
  result=await runWorker(withNormal,2,{angles,center:[1,1,1],up:'z',mirror:n,metric:true});
  assert.equal(result.type,'exported');out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  [...expectedPoint,...expectedNormal].forEach((value,i)=>approx(out.readFloatLE(off+i*4),value));assert.equal(out[off+24],123);
  result=await runWorker(bigFile,1,{mirror:[1,0,0],metric:false});assert.equal(result.type,'exported');
  out=Buffer.from(await result.blob.arrayBuffer());off=out.indexOf('end_header\n')+'end_header\n'.length;
  assert.equal(out.readFloatBE(off),-1);assert.deepEqual(out.subarray(off+12,off+24),big.subarray(12,24));
  result=await runWorker(ascii,1,{mirror:[1,0,0],metric:false});assert.equal(result.type,'exported');
  out=Buffer.from(await result.blob.arrayBuffer()).toString();assert(out.endsWith('-1 2 3 2 7 8 10 20 30\n-4 5 6 0 90 80 70\n3 0 1 2\n'));
  const meshHeader=Buffer.from(`ply\nformat binary_little_endian 1.0\nelement vertex 1\n${normalProps}\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n`);
  result=await runWorker(Buffer.concat([meshHeader,normalData,Buffer.from([0])]),1,{mirror:[1,0,0],metric:false});
  assert.equal(result.type,'error');assert(result.text.includes('дополнительных элементов'));
  result=await runWorker(withNormal,1,{mirror:[0,0,0],metric:false});assert.equal(result.type,'error');
  console.log('Full viewer PLY parsing, metric export, alignment and reflection: PASS');
})().catch(error => { console.error(error); process.exit(1); });
