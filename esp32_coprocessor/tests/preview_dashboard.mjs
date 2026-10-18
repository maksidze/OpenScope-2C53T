// Local-only visual QA of the production page with explicitly synthetic data.
// No instrument ports are opened, no firmware is flashed, no external assets.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import {spawn} from 'node:child_process';
const source=fs.readFileSync(new URL('../src/dashboard_page.hpp',import.meta.url),'utf8');
const page=source.match(/R"OSWEB\(([\s\S]*)\)OSWEB"/)[1];
const mode=process.argv.includes('--meter')?1:0;
const snapshot={api:1,link:true,session:1,mode,mode_age_ms:10,battery_mv:3920,battery_percent:74,
  control:{pending:false,target:0,error:0},meter:{available:true,age_ms:100,revision:125,quality:0,submode:0,result:1,reject:0,flags:2,value:4.998,display:'4.998',unit:'V'},
  scope:{available:true,age_ms:30,revision:200,flags:31,timebase:16,stride:8,sample_rate:12500,
    volts_per_count:[.01408,.00949],tiers:[1,1,2],
    ch1:Array.from({length:128},(_,i)=>Math.round(128+80*Math.sin(i/6))),
    ch2:Array.from({length:128},(_,i)=>Math.round(128+55*Math.sin(i/6+1)))}};
const server=http.createServer((req,res)=>{
  res.setHeader('Cache-Control','no-store');
  if(req.url==='/api/live'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify(snapshot));}
  else {res.setHeader('Content-Type','text/html; charset=utf-8');res.end(page.replace('LIVE / Wi-Fi','TEST DATA / LOCAL PREVIEW'));}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'openscope-web-preview-'));
const shot=path.join(temp,mode?'meter.png':'scope.png');
const edge='C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const child=spawn(edge,['--headless','--disable-gpu','--no-first-run','--disable-background-networking',
  '--disable-extensions','--disable-sync',`--user-data-dir=${path.join(temp,'profile')}`,
  '--window-size=1280,1000','--virtual-time-budget=2500',`--screenshot=${shot}`,
  `http://127.0.0.1:${server.address().port}/`],{windowsHide:true,stdio:'ignore'});
const timer=setTimeout(()=>child.kill(),25000);
try{const code=await new Promise((resolve,reject)=>{child.on('exit',resolve);child.on('error',reject);});
  if(code!==0 || !fs.existsSync(shot))throw Error('Headless preview failed: '+code);
  console.log(shot);
}finally{clearTimeout(timer);server.close();}
