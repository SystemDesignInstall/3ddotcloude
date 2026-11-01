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
    this.dataset={};for(const m of attrs.matchAll(/\bdata-([a-z-]+)="([^"]*)"/g))this.dataset[m[1].replace(/-([a-z])/g,(_,x)=>x.toUpperCase())]=m[2];
  }
  addEventListener(type, callback) { (this.listeners[type] ||= []).push(callback); }
  dispatch(type,extra={}) { if(this.disabled)return; for(const fn of this.listeners[type]||[]) fn({target:this,preventDefault(){},button:0,...extra}); }
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
const axisButtons=Array.from(html.matchAll(/<button([^>]*\bdata-axis="[^"]+"[^>]*)>/g),m=>new Element('', 'button',m[1]));
elements.orientation.querySelectorAll=()=>axisButtons;
elements.units.value='1'; elements['up-axis'].value='y';elements['rotation-step'].value='0.1';
const document={getElementById:id=>elements[id],querySelectorAll:()=>[...Object.values(elements).filter(e=>['BUTTON','INPUT','SELECT'].includes(e.tagName)),...axisButtons],addEventListener(){},pointerLockElement:null};
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
  elements['angle-x'].value='12,34';elements['angle-x'].dispatch('input');elements['angle-z'].value='17,25';elements['angle-z'].dispatch('input');
  vm.runInContext('yaw=.43;pitch=-.21',context);
  const project=()=>JSON.parse(JSON.stringify(vm.runInContext(`(()=>{const [px,py,pz]=rotatePoint([.12,.19,.27]);const cy=Math.cos(yaw),sy=Math.sin(yaw),cp=Math.cos(pitch),sp=Math.sin(pitch),x=cy*px-sy*pz,z=sy*px+cy*pz;return [-x,cp*py-sp*z,sp*py+cp*z]})()`,context)));
  let before=project();elements['opposite-view'].dispatch('click');let after=project();approximate(after,[-before[0],before[1],-before[2]]);
  elements['opposite-view'].dispatch('click');approximate(project(),before);
  elements['mirror-cloud'].dispatch('click');after=project();approximate(after,[-before[0],before[1],before[2]]);assert.equal(elements['mirror-cloud'].attributes['aria-pressed'],'true');assert.deepEqual(fs.readFileSync(cloudPath),initial);
  vm.runInContext('sourceHasOtherElements=true;updateCalibration()',context);assert.equal(elements['mirror-warning'].hidden,false);assert.equal(elements['save-current-alignment'].disabled,true);
  elements['mirror-cloud'].dispatch('click');assert.equal(elements['mirror-cloud'].attributes['aria-pressed'],'false');vm.runInContext('sourceHasOtherElements=false;updateCalibration()',context);approximate(project(),before);
  elements['reset-orientation'].dispatch('click');elements['front-view'].dispatch('click');
  const nudge=(axis,direction)=>axisButtons.find(b=>b.dataset.axis===axis&&b.dataset.step===String(direction)).dispatch('click');
  elements['rotation-step'].value='0.01';nudge('x',1);nudge('x',1);nudge('x',1);
  assert.equal(elements['rotate-x'].value,'0.03');assert.equal(elements['angle-x'].value,'0,03');
  nudge('x',-1);assert.equal(elements['angle-x'].value,'0,02');
  elements['angle-x'].value='12,34';elements['angle-x'].dispatch('input');assert.equal(elements['rotate-x'].value,'12.34');
  elements['angle-x'].dispatch('keydown',{key:'ArrowUp'});assert.equal(elements['angle-x'].value,'12,35');
  elements['angle-x'].dispatch('keydown',{key:'ArrowDown'});assert.equal(elements['angle-x'].value,'12,34');
  elements['angle-x'].value='181';elements['angle-x'].dispatch('input');assert.equal(elements['angle-error'].hidden,false);assert.equal(elements['save-current-alignment'].disabled,true);assert.equal(elements['prepare-aligned'].disabled,true);
  elements['calibration-toggle'].dispatch('click');assert.equal(elements.orientation.hidden,false,'invalid angle must remain visible');
  elements['save-current-alignment'].dispatch('click');assert.deepEqual(fs.readFileSync(cloudPath),initial);
  elements['angle-x'].value='2,345';elements['angle-x'].dispatch('input');assert.equal(elements['save-current-alignment'].disabled,true);
  elements['angle-x'].value='180';elements['angle-x'].dispatch('input');nudge('x',1);assert.equal(elements['angle-x'].value,'180');nudge('x',-1);assert.equal(elements['angle-x'].value,'179,99');
  elements['angle-x'].value='-180';elements['angle-x'].dispatch('input');nudge('x',-1);assert.equal(elements['angle-x'].value,'-180');
  nudge('y',-1);assert.equal(elements['angle-y'].value,'-0,01');elements['angle-z'].value='1,23';elements['angle-z'].dispatch('input');assert.equal(elements['rotate-z'].value,'1.23');
  elements['reset-orientation'].dispatch('click');assert.equal(elements['angle-y'].value,'0');assert.equal(elements['angle-z'].value,'0');
  elements['rotate-x'].value='90';elements['rotate-x'].dispatch('input');
  assert.equal(elements['angle-x'].value,'90');assert.equal(elements['angle-error'].hidden,true);
  elements['up-axis'].value='z';elements['up-axis'].dispatch('change');
  const viewBefore=vm.runInContext('({yaw,pitch,panX,panY})',context);
  elements['save-current-alignment'].dispatch('click');
  assert(elements['rotate-x'].disabled&&elements['angle-x'].disabled,'editing must be locked while writing');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY'));
  assert.equal(elements['rotate-x'].disabled,false);
  const viewAfter=vm.runInContext('({yaw,pitch,panX,panY})',context);
  approximate([viewAfter.yaw,viewAfter.pitch,viewAfter.panX,viewAfter.panY],[viewBefore.yaw,viewBefore.pitch,viewBefore.panX,viewBefore.panY]);
  const transformed=fs.readFileSync(cloudPath);approximate(point(transformed),[0,.5,.5,0,-1,0]);
  assert(transformed.subarray(0,transformed.indexOf('end_header')).includes('comment SpatialPhotoCloud up z'));
  const backups=fs.readdirSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups'));
  assert.equal(backups.length,1);assert.deepEqual(fs.readFileSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups',backups[0])),initial);
  assert.equal(elements['rotate-x'].value,'-90');assert.equal(elements['up-axis'].value,'z');
  elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY')&&!elements['save-current-alignment'].disabled);
  approximate(point(fs.readFileSync(cloudPath)),point(transformed));
  assert.equal(fs.readdirSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups')).length,1);
  elements['angle-x'].value='-89,99';elements['angle-x'].dispatch('input');
  elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY')&&!elements['save-current-alignment'].disabled);
  const finelyRotated=point(fs.readFileSync(cloudPath)),radians=.01*Math.PI/180;
  approximate(finelyRotated,[0,.5*Math.cos(radians),.5+.5*Math.sin(radians),0,-Math.cos(radians),-Math.sin(radians)]);
  assert(Math.abs(finelyRotated[2]-.5)>0.00005,'0.01 degree must change the saved geometry');
  assert.equal(elements['angle-x'].value,'-90');
  const beforeMirror=fs.readFileSync(cloudPath);elements['mirror-cloud'].dispatch('click');assert.equal(elements['mirror-cloud'].attributes['aria-pressed'],'true');assert.deepEqual(fs.readFileSync(cloudPath),beforeMirror);
  elements['save-current-alignment'].dispatch('click');await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY')&&!elements['save-current-alignment'].disabled);
  const mirrored=point(fs.readFileSync(cloudPath));approximate(mirrored,[2,.5*Math.cos(radians),.5+.5*Math.sin(radians),0,-Math.cos(radians),-Math.sin(radians)]);assert.equal(elements['mirror-cloud'].attributes['aria-pressed'],'false');
  assert.deepEqual(fs.readFileSync(path.join(path.dirname(cloudPath),'.SpatialPhotoCloud-backups',backups[0])),initial);
  // An asymmetric cloud gets a different bounding-box centre after a general
  // rotation/reflection. Saving and reopening must keep its screen position.
  elements['angle-x'].value='-80,4';elements['angle-x'].dispatch('input');elements['angle-y'].value='23';elements['angle-y'].dispatch('input');elements['angle-z'].value='35';elements['angle-z'].dispatch('input');
  vm.runInContext('yaw=.43;pitch=-.21;panX=.13;panY=-.09;zoom=1.4',context);elements['mirror-cloud'].dispatch('click');
  const screenPoints=()=>JSON.parse(JSON.stringify(vm.runInContext(`(()=>{const result=[],cy=Math.cos(yaw),sy=Math.sin(yaw),cp=Math.cos(pitch),sp=Math.sin(pitch),aspect=canvas.clientWidth/canvas.clientHeight,rx=aspect>=1?1/aspect:1,ry=aspect>=1?1:aspect;for(let i=0;i<count;i++){const [px,py,pz]=rotatePoint(Array.from(pointPositions.slice(3*i,3*i+3)));const x=cy*px-sy*pz,z=sy*px+cy*pz,y=cp*py-sp*z;result.push(-x*rx*zoom+panX,y*ry*zoom+panY)}return result})()`,context)));
  const screenBefore=screenPoints();elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('Сохранено в исходный PLY')&&!elements['save-current-alignment'].disabled);approximate(screenPoints(),screenBefore);
  const external=Buffer.from(fs.readFileSync(cloudPath));external[external.length-1]^=1;fs.writeFileSync(cloudPath,external);
  fs.utimesSync(cloudPath,new Date(Date.now()+10000),new Date(Date.now()+10000));
  elements['save-current-alignment'].dispatch('click');
  await waitFor(()=>elements.status.textContent.includes('PLY изменился вне программы'));
  assert.deepEqual(fs.readFileSync(cloudPath),external);assert.equal(elements['save-current-alignment'].disabled,false);
  console.log('Application viewer saved in place, preserved the original backup and reopened the result: PASS');
})().catch(error=>{console.error(error);process.exit(1);});
