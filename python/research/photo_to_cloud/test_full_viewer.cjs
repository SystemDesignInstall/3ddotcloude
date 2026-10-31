// Tests the exact offline worker embedded in the full viewer; no browser or network is required.
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, 'full_viewer.html'), 'utf8');
const match = html.match(/<script id="parser-worker" type="text\/x-worker">([\s\S]*?)<\/script>/);
assert(match, 'PLY worker missing');
async function parse(buffer) {
  const messages = [];
  const context = {self: {postMessage: value => messages.push(value)}, TextDecoder, Uint8Array,
                   Float32Array, Float64Array, DataView, Number, Math, Object, Error, String, Infinity};
  vm.createContext(context);
  vm.runInContext(match[1], context);
  await context.self.onmessage({data: {arrayBuffer: async () => buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength)}});
  return messages[messages.length - 1];
}
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
  console.log('Full viewer PLY parser: PASS');
})().catch(error => { console.error(error); process.exit(1); });
