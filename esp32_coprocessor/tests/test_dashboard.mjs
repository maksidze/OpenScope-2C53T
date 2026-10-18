// Executes the production inline JavaScript: no network, browser libraries or
// firmware flashing needed. Canvas and fetch are observable platform edges.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const header=fs.readFileSync(new URL('../src/dashboard_page.hpp',import.meta.url),'utf8');
const script=header.match(/<script>([\s\S]*?)<\/script>/)[1];
let checks=0;const check=(condition)=>{assert.ok(condition);checks++;};
const nodes=new Map(),requests=[];let strokes=0,segments=0,confirm=true;
const canvasContext={clearRect(){},beginPath(){},moveTo(){},lineTo(){segments++;},stroke(){strokes++;}};
const node=id=>{
  if(!nodes.has(id))nodes.set(id,{textContent:'',className:'',hidden:false,disabled:false,
    clientWidth:1000,clientHeight:360,width:0,height:0,dataset:{},setAttribute(k,v){this[k]=v;},
    addEventListener(){},getContext(){return canvasContext;}});
  return nodes.get(id);
};
const buttons=[0,1,2,3].map(n=>{const b=node('button-'+n);b.dataset.mode=String(n);return b;});
const state={api:1,link:true,session:1,mode:1,mode_age_ms:10,battery_mv:3900,battery_percent:70,
  control:{pending:false,target:0,error:0},
  meter:{available:true,age_ms:10,revision:12,quality:0,submode:2,result:1,reject:0,flags:0,value:4.7,display:'4.700',unit:'mA'},
  scope:{available:true,age_ms:10,revision:40,flags:31,timebase:16,stride:8,sample_rate:12500,
    volts_per_count:[.01408,.00949],tiers:[1,1,2],
    ch1:Array.from({length:128},(_,i)=>Math.round(128+70*Math.sin(i/6))),
    ch2:Array.from({length:128},(_,i)=>Math.round(128+50*Math.cos(i/6)))}};
let fetchError=false;
const context=vm.createContext({console,AbortSignal:{timeout(){}},setTimeout(){},Date,
  document:{getElementById:node,querySelectorAll(){return buttons;},hidden:false},
  window:{devicePixelRatio:1,confirm(){return confirm;},addEventListener(){}},
  async fetch(url,options){requests.push({url,options});if(fetchError)throw Error('offline');return {ok:true,json:async()=>url==='/api/live'?state:{queued:true}};}});
vm.runInContext(script,context);
await new Promise(setImmediate);await new Promise(setImmediate);
check(requests[0].url==='/api/live');check(node('meter-value').textContent==='4.700');
check(node('meter-unit').textContent==='mA');check(node('meter-detail').textContent.startsWith('DC mA'));
check(buttons[1]['aria-pressed']==='true' && !buttons[0].disabled);
const render=()=>vm.runInContext(`render(${JSON.stringify(state)})`,context);
state.meter.quality=2;render();check(node('meter-value').textContent==='OL');
state.meter.quality=5;render();check(node('meter-value').textContent==='—');
state.meter.quality=0;state.meter.age_ms=3001;render();check(node('meter-value').textContent==='—');
state.meter.flags=1;render();check(node('meter-state').textContent==='HOLD');
state.mode=0;render();check(node('meter').hidden && !node('scope').hidden);
check(segments>=250 && strokes>=3);check(node('channel-1').textContent.includes('~0.01408'));
check(node('time-axis').textContent.includes('81.28')); // 127 intervals * 8 / 12500
state.scope.sample_rate=null;state.scope.volts_per_count=[null,null];render();
check(node('time-axis').textContent.includes('1016'));check(node('channel-1').textContent.includes('неизвестен'));
const beforeStale=strokes;state.scope.age_ms=5000;render();check(strokes===beforeStale+1); // grid only
check(node('scope-state').textContent.includes('не обновляется'));
state.scope.flags&=~2;render();check(node('scope-state').textContent.startsWith('STOP'));
state.mode=2;render();check(node('other-text').textContent.includes('генератор'));
state.mode=3;render();check(node('other-text').textContent.includes('настройки'));
state.link=false;render();check(buttons.every(b=>b.disabled));check(node('scope').hidden && node('meter').hidden);
state.link=true;state.mode_age_ms=4000;render();check(buttons.every(b=>b.disabled));
state.mode_age_ms=0;state.control.pending=true;render();check(buttons.every(b=>b.disabled));
check(node('message').textContent.includes('Переключение'));
state.control.pending=false;state.control.error=3;render();check(node('message').textContent.includes('занят'));
state.control.error=0;render();await vm.runInContext('switchMode(1)',context);
check(requests.at(-1).options.method==='POST');check(requests.at(-1).options.body==='mode=1');
check(requests.at(-1).options.headers['X-OpenScope-Control']==='1');
confirm=false;let count=requests.length;await vm.runInContext('switchMode(2)',context);check(requests.length===count);
fetchError=true;await vm.runInContext('poll()',context);check(node('connection').textContent.includes('недоступна'));
check(node('scope').hidden && node('meter').hidden && buttons.every(b=>b.disabled));
fetchError=false;state.mode=1;state.meter.age_ms=1;state.meter.flags=0;await vm.runInContext('poll()',context);
check(node('meter-value').textContent==='4.700' && !buttons[0].disabled);
console.log(`Production dashboard JavaScript: ${checks} checks passed`);
