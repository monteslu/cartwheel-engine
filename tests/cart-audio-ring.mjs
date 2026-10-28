// ABI-level regression for a rendered audio burst longer than the old 4096
// frame ring. Requires local romdev and any audio-enabled engine cart.
// The default cart is the dual hello cart (it plays sound), run on GL (wgpu: false).
// node tests/cart-audio-ring.mjs [--cart build/cart-wgpu/carts/hello.wasc]
//     [--before /path/to/old-cart.wasc]
import assert from 'node:assert/strict';
import {resolve} from 'node:path';
import {mkdirSync,writeFileSync} from 'node:fs';
import { wgpuPin } from '../tools/lib/wgpu-pin.mjs';
const root=resolve(import.meta.dirname,'..'),argv=process.argv.slice(2),arg=k=>argv[argv.indexOf(k)+1],cart=argv.includes('--cart')?resolve(arg('--cart')):resolve(root,'build/cart-wgpu/carts/hello.wasc'),out=resolve(root,'out/gate');mkdirSync(out,{recursive:true});
const base=process.env.ROMDEV_URL||`http://127.0.0.1:${process.env.ROMDEV_PORT||7331}`,headers={'content-type':'application/json','x-romdev-session':'cartwheel-cart-audio-ring'};
async function call(n,a){const r=await fetch(base+'/tool/'+n,{method:'POST',headers,body:JSON.stringify(a)}),j=await r.json();if(!r.ok||j.error)throw Error(JSON.stringify(j));return j;}
const read=async(offset,length)=>{const r=await call('wasm',{op:'read',offset,length,inline:true}),b=Buffer.from(r.hex,'hex');assert.equal(b.length,length);return b;};
async function probe(path,ms){try{
 await call('loadMedia',{platform:'wasmcart',gpuPower:wgpuPin().power,wgpu:false,path,deterministicStepMs:ms,deterministicSeed:7});await call('frame',{op:'step',frames:20});const info=(await call('wasm',{op:'info'})).info;
 const before=(await read(info.audioWritePtr,4)).readUInt32LE();await call('frame',{op:'step',frames:1});const after=(await read(info.audioWritePtr,4)).readUInt32LE(),generated=(after-before)>>>0,rate=info.audioSampleRate||48000;
 assert.equal(generated,Math.min(8192,Math.floor(rate*ms/1000)),'host delta must reach the real audio plan');
 assert(generated<=info.audioCap,`${ms}ms frame overwrites its own unread samples (${generated} > ${info.audioCap})`);assert(info.audioCap>8192,'ring must hold more than the maximum generated frame');assert.equal(info.audioCap&(info.audioCap-1),0,'ABI ring is a power of two');
 const end=info.audioPtr+info.audioCap*2*4;assert(info.audioWritePtr>=end,'write cursor must not overlap the audio allocation');for(const name of ['inputPtr','savePtr','timePtr','hostInfoPtr','keysPtr','pointerPtr']){const p=info[name];assert(!p||p<info.audioPtr||p>=end,`${name} must not overlap audio`);}
 await read(info.audioPtr,info.audioCap*8);const conforms=await call('wasm',{op:'conformance'});assert(conforms.conforms);return{frameMs:ms,sampleRate:rate,generatedFrames:generated,audioCap:info.audioCap,audioPtr:info.audioPtr,audioEnd:end,audioWritePtr:info.audioWritePtr,conforms:true};
 }finally{await call('host',{op:'unload'});}}
const report={cart,frames:[]};
if(argv.includes('--before')){let failed=false;try{await probe(resolve(arg('--before')),100);}catch(e){if(e.code!=='ERR_ASSERTION'||!e.message.startsWith('100ms frame overwrites its own unread samples'))throw e;failed=true;report.controlError=e.message;}assert(failed,'old undersized ring must fail the regression');}
for(const ms of [17,86,100,170,250]){report.frames.push(await probe(cart,ms));console.log('PASS audio frame',ms,'ms');}
writeFileSync(resolve(out,'cart-audio-ring.json'),JSON.stringify(report,null,2));console.log('PASS cart audio burst capacity, allocation and ABI',report);
