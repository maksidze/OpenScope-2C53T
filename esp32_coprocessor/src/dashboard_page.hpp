#pragma once
#include <Arduino.h>
namespace openscope {
static const char kDashboardPage[] PROGMEM=R"OSWEB(<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenScope • Live</title><style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0b111a;color:#e1e8f0}
*{box-sizing:border-box}body{max-width:1180px;margin:auto;padding:24px}header{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}
h1{font-size:24px;letter-spacing:.05em;margin:0}h1 small{font-size:12px;color:#55d6be}a{color:#8cd8ff}p{color:#9cacbf;line-height:1.5}
nav{display:flex;gap:8px;flex-wrap:wrap;margin:24px 0}button{font:inherit;border:1px solid #33465d;background:#172435;color:#e1e8f0;border-radius:8px;padding:11px 16px;cursor:pointer}
button:hover{border-color:#55d6be}button[aria-pressed=true]{background:#16483f;border-color:#55d6be}button:disabled{opacity:.45;cursor:wait}button:focus-visible,a:focus-visible{outline:3px solid #8cd8ff;outline-offset:3px}
.card{background:#121d2b;border:1px solid #28394e;border-radius:12px;padding:20px;margin:16px 0}.row{display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
.muted{color:#9cacbf}.status{color:#55d6be}.bad{color:#ffad81}.reading{font:clamp(36px,8vw,80px) ui-monospace,monospace;overflow-wrap:anywhere;margin:24px 0}
.reading small{font-size:.4em;color:#55d6be}canvas{width:100%;height:360px;display:block;border-radius:8px;background:#0b111a;margin-top:16px}
.legend{display:flex;gap:20px;flex-wrap:wrap;margin-top:16px;font:14px ui-monospace,monospace}.ch1{color:#ffd269}.ch2{color:#6bcaff}#message{min-height:24px}footer{font-size:13px;color:#8295ad;margin:24px 0}.note{font-size:13px} [hidden]{display:none!important}
@media(max-width:600px){body{padding:14px}.card{padding:14px}canvas{height:280px}nav button{flex:1}}
</style></head><body>
<header><h1>OpenScope <small>LIVE / Wi-Fi</small></h1><a href="/wifi-settings">Настройки Wi-Fi</a></header>
<p>Измерения активного режима прибора. Данные передаются числами, осциллограмму рисует ваш браузер.</p>
<div class="row"><span id="connection" role="status">Подключение к ESP32…</span><span id="battery" class="muted"></span></div>
<nav aria-label="Режим прибора"><button data-mode="0" disabled>Осциллограф</button><button data-mode="1" disabled>Мультиметр</button><button data-mode="2" disabled>Генератор</button><button data-mode="3" disabled>Настройки</button></nav>
<div id="message" class="bad" role="status"></div>
<section id="meter" class="card" hidden><div class="row"><span>МУЛЬТИМЕТР</span><span id="meter-state" class="muted"></span></div>
<div class="reading"><span id="meter-value">—</span> <small id="meter-unit"></small></div><div id="meter-detail" class="muted"></div></section>
<section id="scope" class="card" hidden><div class="row"><span>ОСЦИЛЛОГРАММА</span><span id="scope-state" class="muted"></span></div>
<canvas id="wave" aria-label="Осциллограмма двух каналов" role="img"></canvas>
<div class="legend"><span id="channel-1" class="ch1">CH1</span><span id="channel-2" class="ch2">CH2</span></div>
<p id="time-axis" class="note"></p><p class="note">Предпросмотр: 128 точек на канал, каждый 8-й отсчёт записи, до 5 кадров/с. Узкие импульсы и быстрые сигналы могут быть пропущены или отображаться с наложением частот. Не заменяет полную запись.</p>
</section>
<section id="other" class="card"><span id="other-text">Ожидание состояния AT32…</span></section>
<footer>Локальное управление без авторизации — только для доверенной сети. Не открывайте порт ESP32 в интернет. Закрытие страницы отключает фоновую передачу, если её не включило другое приложение.</footer>
<script>
'use strict';
const el=id=>document.getElementById(id), buttons=Array.from(document.querySelectorAll('[data-mode]'));
const names=['Осциллограф','Мультиметр','Генератор','Настройки'];
const meterNames=['DC V','AC V','DC mA','DC A','AC mA','AC A','Сопротивление','Прозвонка','Диод','Ёмкость','Температура'];
let latest=null, busy=false, networkError='', lastGood=0;
function fmt(v,digits=4){return Number.isFinite(v)?Number(v.toPrecision(digits)).toString():'—';}
function render(s){
  latest=s;lastGood=Date.now();
  const live=s.link && s.mode!==null && s.mode_age_ms<3000;
  el('connection').textContent=s.link?(s.mode===null?'AT32 на связи • ожидаем режим':names[s.mode]+' • AT32 на связи'):'ESP32 на связи • AT32 offline';
  el('connection').className=live?'status':'bad';
  el('battery').textContent=s.battery_mv?s.battery_percent+'% · '+fmt(s.battery_mv/1000)+' V':'';
  buttons.forEach(b=>{b.disabled=!live || busy || s.control.pending;b.setAttribute('aria-pressed',String(live && Number(b.dataset.mode)===s.mode));});
  const errors={1:'AT32 отклонил команду.',2:'Прошивка AT32 не поддерживает эту команду.',3:'AT32 занят: закройте модальное окно на приборе.',4:'Связь с AT32 потеряна.',5:'AT32 не подтвердил переключение режима.'};
  el('message').textContent=networkError || (s.control.pending?'Переключение на «'+names[s.control.target]+'»…':errors[s.control.error] || '');
  el('meter').hidden=!live || s.mode!==1;el('scope').hidden=!live || s.mode!==0;el('other').hidden=live && (s.mode===0 || s.mode===1);
  el('other-text').textContent=!live?'Ожидание свежих данных AT32. Проверьте питание и UART.':s.mode===2?'Активен генератор. Его параметры задаются на приборе.':'На приборе открыты настройки.';
  if(live && s.mode===1){
    const m=s.meter, held=!!(m.flags&1), stale=m.age_ms>2000 && !held;
    const usable=m.available && !stale;
    el('meter-value').textContent=!usable?'—':m.quality===2?'OL':m.quality===0?m.display:'—';
    el('meter-unit').textContent=usable?m.unit:'';
    el('meter-state').textContent=!m.available?'Нет показаний':stale?'Данные устарели':held?'HOLD':m.quality===0?'LIVE':m.quality===2?'Перегрузка':'Недостоверное показание';
    el('meter-detail').textContent=(meterNames[m.submode] || 'Режим '+m.submode)+' · кадр '+m.revision+' · '+m.age_ms+' мс'+(m.reject?' · причина отклонения '+m.reject:'');
  }
  if(live && s.mode===0)drawScope(s.scope);
}
function drawScope(w){
  const canvas=el('wave'),ratio=window.devicePixelRatio || 1;
  const width=Math.max(1,Math.round(canvas.clientWidth*ratio)),height=Math.max(1,Math.round(canvas.clientHeight*ratio));
  if(canvas.width!==width || canvas.height!==height){canvas.width=width;canvas.height=height;}
  const c=canvas.getContext('2d'),pad=30*ratio,W=width-2*pad,H=height-2*pad;
  c.clearRect(0,0,width,height);c.strokeStyle='#24354a';c.lineWidth=ratio;
  c.beginPath();for(let i=0;i<=10;i++){let x=pad+W*i/10;c.moveTo(x,pad);c.lineTo(x,height-pad);}for(let i=0;i<=8;i++){let y=pad+H*i/8;c.moveTo(pad,y);c.lineTo(width-pad,y);}c.stroke();
  const stale=w.age_ms>2000 && !!(w.flags&2),ok=w.available && !stale;
  el('scope-state').textContent=!w.available?'Ожидание реальной записи FPGA':stale?'Запись не обновляется':w.flags&2?'RUN · кадр '+w.revision:'STOP · кадр '+w.revision;
  for(let ch=0;ch<2;ch++){
    const a=ch?w.ch2:w.ch1,enabled=!!(w.flags&(4<<ch)),label=el('channel-'+(ch+1));
    if(!ok || !enabled || !a.length){label.textContent='CH'+(ch+1)+(enabled?' · нет данных':' · выключен');continue;}
    let lo=Math.min(...a),hi=Math.max(...a),span=Math.max(8,hi-lo),mid=(lo+hi)/2;
    const top=pad+H*ch/2,band=H/2;c.strokeStyle=ch?'#6bcaff':'#ffd269';c.lineWidth=1.6*ratio;c.beginPath();
    a.forEach((v,i)=>{const x=pad+W*i/(a.length-1),y=top+band/2-(v-mid)/span*band*.8;i?c.lineTo(x,y):c.moveTo(x,y);});c.stroke();
    const scale=w.volts_per_count[ch],tier=w.tiers[ch];
    label.textContent='CH'+(ch+1)+' · '+lo+'…'+hi+' ADC · '+(scale?(tier===1?'~':'')+fmt(scale)+' V/отсчёт':'масштаб V неизвестен');
  }
  let text=w.sample_rate?'Время: '+(w.tiers[2]===1?'~':'')+fmt(127*w.stride/w.sample_rate*1000)+' мс на запись · '+fmt(w.sample_rate)+' отсчётов/с':'Время: 0…1016 исходных отсчётов (частота выборки не откалибрована)';
  el('time-axis').textContent=text+' · Вертикаль: автоподбор по ADC, не V/дел.'+(!(w.flags&16)?' Порядок записи не подтверждён.':'');
}
async function switchMode(mode){
  if(mode===2 && !window.confirm('Переключить прибор в режим генератора? Проверьте подключение выхода и параметры на приборе.'))return;
  busy=true;networkError='';buttons.forEach(b=>b.disabled=true);
  try{
    const r=await fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-OpenScope-Control':'1'},body:'mode='+mode,signal:AbortSignal.timeout(4000)});
    const reply=await r.json();if(!r.ok)throw new Error(reply.error || 'HTTP '+r.status);
    el('message').textContent='Команда принята. Ожидаем подтверждения AT32…';
  }catch(e){networkError='Не удалось переключить режим: '+e.message;el('message').textContent=networkError;}
  finally{busy=false;}
}
buttons.forEach(b=>b.addEventListener('click',()=>switchMode(Number(b.dataset.mode))));
async function poll(){
  if(!document.hidden){
    try{
      const r=await fetch('/api/live',{cache:'no-store',signal:AbortSignal.timeout(3000)});
      if(!r.ok)throw new Error('HTTP '+r.status);render(await r.json());
    }catch(e){
      el('connection').textContent='ESP32 недоступна: '+e.message;el('connection').className='bad';buttons.forEach(b=>b.disabled=true);
      el('meter').hidden=true;el('scope').hidden=true;el('other').hidden=false;el('other-text').textContent='Ожидаем восстановления связи. Последние показания скрыты.';
    }
  }
  setTimeout(poll,document.hidden?1000:250);
}
window.addEventListener('resize',()=>{if(latest && latest.mode===0 && Date.now()-lastGood<3000)drawScope(latest.scope);});
poll();
</script></body></html>)OSWEB";
}
