// Feed the actual imported PLY to the production viewer parser.
const fs = require('fs'), vm = require('vm'), path = require('path'), assert = require('assert');
const html = fs.readFileSync(path.join(__dirname, 'full_viewer.html'), 'utf8');
const source = html.match(/<script id="parser-worker" type="text\/x-worker">([\s\S]*?)<\/script>/)[1];
const messages = [];
const context = {self: {postMessage: value => messages.push(value)}, TextDecoder, TextEncoder, Blob,
  Uint8Array, Uint32Array, Float32Array, Float64Array, DataView, Number, Math, Object, Error, String, Infinity};
vm.createContext(context);
vm.runInContext(source, context);
const bytes = fs.readFileSync(process.argv[2]);
(async () => {
  await context.self.onmessage({data: {arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)}});
  const result = messages.at(-1);
  assert.equal(result.type, 'done', result.text);
  assert.equal(result.valid, 3);
  assert.equal(result.metric, true);
  assert.equal(result.up, 'z');
  const positions = new Float32Array(result.positions), colors = new Uint8Array(result.colors);
  assert(Math.abs((positions[3] - positions[0]) * result.scale - .001) < 1e-6, 'millimetre spacing lost');
  assert.deepEqual(Array.from(colors), [255, 0, 0, 0, 255, 0, 128, 128, 255]);
})().catch(error => {console.error(error); process.exitCode = 1;});
