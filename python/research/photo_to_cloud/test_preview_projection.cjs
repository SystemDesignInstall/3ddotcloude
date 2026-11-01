// Exercise the actual generated preview's projection and depth ordering.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(process.argv[2],'utf8');
let source=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
source=source.replace(/const points=\[[^\n]*\], canvas=/,'const points=[[.2,0,0,255,0,0],[-.2,0,0,0,255,0],[0,0,-.2,0,0,255],[0,0,.2,255,255,0]], canvas=');
const draws=[],ctx={fillStyle:'',setTransform(){},fillRect(x,y,w,h){draws.push({x,y,color:this.fillStyle});}};
const canvas={clientWidth:100,clientHeight:100,getContext:()=>ctx,addEventListener(){}};
const context={document:{getElementById:()=>canvas},window:{devicePixelRatio:1,addEventListener(){}},requestAnimationFrame:()=>1,Math};
vm.createContext(context);vm.runInContext(source,context);vm.runInContext('yaw=0;pitch=0;draw()',context);
const red=draws.find(p=>p.color==='rgb(255,0,0)'),green=draws.find(p=>p.color==='rgb(0,255,0)');
assert(red.x<50&&green.x>50,'camera right must agree with the full viewer');
const far=draws.findIndex(p=>p.color==='rgb(255,255,0)'),near=draws.findIndex(p=>p.color==='rgb(0,0,255)');
assert(far>=0&&near>far,'far points must be drawn before near points');
console.log('Preview projection and depth ordering: PASS');
