// Exercise the real viewer scripts and worker against the local host, with a
// minimal DOM/WebGL shim so this test runs without a desktop browser.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const path = require('path');
const html = fs.readFileSync(path.join(__dirname, 'full_viewer.html'), 'utf8');
const workerSource = html.match(/<script id="parser-worker" type="text\/x-worker">([\s\S]*?)<\/script>/)[1];
const mainSource = html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const address = new URL(process.argv[2]);
const cloudPath = process.argv[3];
const initial = fs.readFileSync(cloudPath);

class Element {
  constructor(id, tag, attrs) {
    this.id=id; this.tagName=tag.toUpperCase(); this.hidden=/\shidden(?:\s|>|=)/.test(attrs);
    this.disabled=/\sdisabled(?:\s|>|=)/.test(attrs); this.value=(attrs.match(/\bvalue="([^"]*)"/)||[])[1]||'';
    this.textContent=''; this.className=''; this.style={}; this.listeners={}; this.attributes={};
    this.clientWidth=1200; this.clientHeight=700; this.width=1200; this.height=700;
  }
  addEventListener(type, callback) { (this.listeners[type] ||= []).push(callback); }
  dispatch(type) { if(this.disabled)return; for(const fn of this.listeners[type]||[]) fn({target:this,preventDefault(){},button:0}); }
  setAttribute(name,value) { this.attributes[name]=value; }
  removeAttribute(name) { delete this.attributes[name]; }
  querySelectorAll() { return []; }
  focus() {}
  getContext() { return gl; }
}
const gl = new Proxy({NO_ERROR:0,COMPILE_STATUS:1,LINK_STATUS:2,FRAMEBUFFER_COMPLETE:3,
  getShaderParameter:()=>true,getProgramParameter:()=>true,getError:()=>0,
  getAttribLocation:()=>0,getUniformLocation:()=>({}),createShader:()=>({}),createProgram:()=>({}),
  createBuffer:()=>({}),checkFramebufferStatus:()=>3},
  {get:(target,key)=>key in target?target[key]:(()=>{})});
const elements={};
for(const match of html.matchAll(/<(\w+)([^>]*\bid="([^"]+)"[^>]*)>/g)) elements[match[3]]=new Element(match[3],match[1],match[2]);
elements.units.value='1'; elements['up-axis'].value='y';
const document={getElementById:id=>elements[id],querySelectorAll:()=>Object.values(elements).filter(e=>['BUTTON','INPUT','SELECT'].includes(e.tagName)),addEventListener(){},pointerLockElement:null};
const localFetch = (url,options={})=>{
  const headers=new Headers(options.headers||{});
  if(options.method&&options.method!=='GET')headers.set('Origin',address.origin);
  return fetch(new URL(url,address),{...options,headers});
};
class WorkerMock {
  constructor(){
    this.dead=false;
    const self={location:{origin:address.origin},postMessage:data=>setTimeout(()=>{if(!this.dead&&this.onmessage)this.onmessage({data});},0)};
    this.context=vm.createContext({self,TextDecoder,TextEncoder,Uint8Array,Float32Array,Float64Array,DataView,Number,Math,Object,Error,String,Infinity,Set,Blob,URL,fetch:localFetch});
    vm.runInContext(workerSource,this.context);
  }
  postMessage(data){setTimeout(async()=>{try{await this.context.self.onmessage({data});}catch(error){if(!this.dead&&this.onerror)this.onerror(error);}},0);}
  terminate(){this.dead=true;}
}
const context={document,window:{devicePixelRatio:1,addEventListener(){}},location:address,URL,URLSearchParams,Blob,Worker:WorkerMock,
  fetch:localFetch,requestAnimationFrame:()=>1,Intl,Math,Number,Object,Set,Array,Uint8Array,Float32Array,console};
vm.createContext(context);vm.runInContext(mainSource,context);
async function waitFor(test){const end=Date.now()+15000;while(Date.now()<end){if(test())return;await new Promise(r=>setTimeout(r,10));}throw Error('Timed out: '+elements.status.textContent);}
function point(buffer){const off=buffer.indexOf('end_header\n')+'end_header\n'.length;return [0,1,2,3,4,5].map(i=>buffer.readFloatLE(off+i*4));}
function approximate(actual,expected){actual.forEach((v,i)=>assert(Math.abs(v-expected[i])<1e-5,`${v} != ${expected[i]}`));}

(async()=>{
  await waitFor(()=>elements.status.textContent.includes('Загружено точек:'));
  assert.equal(elements['save-current-alignment'].hidden,false);
  elements['orientation-toggle'].dispatch('click');
  elements['rotate-x'].value='90';elements['rotate-x'].dispatch('input');
  elements['up-axis'].value='z';elements['up-axis'].dispatch('change');
  const viewBefore=vm.runInContext('({yaw,pitch,panX,panY})',context);
  elements['save-current-alignment'].dispatch('click');
  assert(elements['rotate-x'].disabled,'editing must be locked while writing');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY'));
  assert.equal(elements['rotate-x'].disabled,false);
  assert.deepEqual(JSON.parse(JSON.stringify(vm.runInContext('({yaw,pitch,panX,panY})',context))),JSON.parse(JSON.stringify(viewBefore)));
  const transformed=fs.readFileSync(cloudPath);approximate(point(transformed),[0,.5,.5,0,-1,0]);
  assert(transformed.subarray(0,transformed.indexOf('end_header')).includes('comment SpatialPhotoCloud up z'));
  const backups=fs.readdirSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups'));
  assert.equal(backups.length,1);assert.deepEqual(fs.readFileSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups',backups[0])),initial);
  assert.equal(elements['rotate-x'].value,'-90');assert.equal(elements['up-axis'].value,'z');
  elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY')&&!elements['save-current-alignment'].disabled);
  approximate(point(fs.readFileSync(cloudPath)),point(transformed));
  assert.equal(fs.readdirSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups')).length,1);
  const external=Buffer.from(fs.readFileSync(cloudPath));external[external.length-1]^=1;fs.writeFileSync(cloudPath,external);
  fs.utimesSync(cloudPath,new Date(Date.now()+10000),new Date(Date.now()+10000));
  elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('PLY изменился вне программы'));
  assert.deepEqual(fs.readFileSync(cloudPath),external);assert.equal(elements['save-current-alignment'].disabled,false);
  console.log('Application viewer saved in place, preserved the original backup and reopened the result: PASS');
})().catch(error=>{console.error(error);process.exit(1);});
