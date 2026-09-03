// ============================================================================
// web_page.h — статическая веб-морда (HTML+CSS+JS одним файлом), лежит
// в flash (PROGMEM). Никаких внешних CDN — плата в поле без интернета.
// Общается с прошивкой через простые GET-эндпоинты (см. комментарии внизу
// файла и раздел 6 README). Это одностраничное приложение без сборки.
// ============================================================================
#pragma once

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>C6-Tracker</title>
<style>
  :root{color-scheme:dark;--bg:#12151a;--panel:#1b1f27;--line:#2a2f3a;--txt:#e7ebf3;
        --dim:#8a93a6;--acc:#4fb0ff;--good:#3ddc84;--warn:#ffb84f;--bad:#ff5f5f;}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.4 -apple-system,Segoe UI,Roboto,sans-serif;
       padding:10px;max-width:720px;margin:0 auto;}
  h1{font-size:17px;margin:10px 0 4px}
  h2{font-size:13px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em;margin:16px 0 6px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px;margin-bottom:10px}
  .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
  button{background:#232838;color:var(--txt);border:1px solid var(--line);border-radius:8px;
         padding:9px 12px;font-size:14px;flex:1 1 auto;min-width:84px}
  button:active{background:var(--acc);color:#08131f}
  button.primary{background:var(--acc);color:#08131f;border-color:var(--acc)}
  button.stop{background:var(--bad);color:#1a0505;border-color:var(--bad)}
  select,input{background:#0e1116;color:var(--txt);border:1px solid var(--line);border-radius:6px;
               padding:6px;font-size:13px;width:100%}
  label{font-size:12px;color:var(--dim);display:block;margin-bottom:2px}
  table{width:100%;border-collapse:collapse;font-size:12px}
  th,td{text-align:left;padding:4px 4px;border-bottom:1px solid var(--line)}
  th{color:var(--dim);font-weight:normal}
  tr.sel{background:#1d2e22}
  .stat{display:flex;justify-content:space-between;padding:2px 0;font-size:13px}
  .stat b{color:var(--acc)}
  .grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  canvas{width:100%;height:auto;background:#0a0c10;border-radius:8px;border:1px solid var(--line)}
  .legend{display:flex;height:14px;border-radius:4px;overflow:hidden;margin-top:6px}
  .legend div{flex:1}
  .small{font-size:11px;color:var(--dim)}
  .pill{display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;background:#232838}
  .pill.on{background:var(--good);color:#04220f}
  a.dl{color:var(--acc);text-decoration:none;font-size:13px}
</style>
</head>
<body>
<h1>C6-Tracker — направленная антенна</h1>
<div class="row small">
  <span class="pill" id="pState">state: —</span>
  <span class="pill" id="pMode">режим: —</span>
</div>

<div class="card">
  <h2>Текущее положение</h2>
  <div class="stat">Pan (азимут) <b id="vPan">—</b></div>
  <div class="stat">Tilt (угол места) <b id="vTilt">—</b></div>
  <div class="stat">RSSI BLE-цели <b id="vRssiBle">—</b></div>
  <div class="stat">RSSI Wi-Fi-цели <b id="vRssiWifi">—</b></div>
  <div class="row" style="margin-top:8px">
    <button onclick="cmd('park')">Парковка</button>
    <button class="stop" onclick="cmd('stop')">СТОП</button>
  </div>
</div>

<div class="card">
  <h2>Режим радио</h2>
  <div class="row">
    <button id="mBle" onclick="setMode('ble')">BLE</button>
    <button id="mWifi" onclick="setMode('wifi')">Wi-Fi</button>
  </div>
  <p class="small">SoftAP и скан делят один физический радиомодуль. Одновременная
  работа BLE и Wi-Fi на этом чипе — самый нестабильный режим из всех
  проверенных (см. README) — режим "оба сразу" намеренно убран, выбирай
  ровно одно.</p>
</div>

<div class="card">
  <h2>Wi-Fi точки доступа</h2>
  <div class="row">
    <button onclick="wifiFullscan()">Полный скан 1..13 (в парковке)</button>
  </div>
  <p class="small" id="wifiScanStatus"></p>
  <table id="wifiTable"><thead><tr><th>SSID</th><th>BSSID</th><th>CH</th><th>RSSI</th><th></th></tr></thead>
  <tbody></tbody></table>
</div>

<div class="card">
  <h2>BLE устройства</h2>
  <div class="row">
    <button onclick="cmd('start&what=ble_fullscan')">Полный BLE-скан (по всей сетке)</button>
  </div>
  <p class="small">Колонка "Лучший угол" копится пассивно всё время (не только
  во время полного скана) — держит pan/tilt, на котором конкретное устройство
  ловилось сильнее всего с момента загрузки платы. Живёт в памяти, не в NVS —
  пропадает при перезагрузке.</p>
  <table id="bleTable"><thead><tr><th>Имя</th><th>MAC</th><th>RSSI</th><th>Лучший угол</th><th></th></tr></thead>
  <tbody></tbody></table>
</div>

<div class="card">
  <h2>Цель и наведение</h2>
  <div class="stat">BLE-цель <b id="tBle">не выбрана</b></div>
  <div class="stat">Wi-Fi-цель <b id="tWifi">не выбрана</b></div>
  <div class="row" style="margin-top:8px">
    <button onclick="cmd('start&what=coarse')">Обзор (грубый растр)</button>
    <button class="primary" onclick="cmd('start&what=track_ble')">Навести: BLE</button>
    <button class="primary" onclick="cmd('start&what=track_wifi')">Навести: Wi-Fi</button>
  </div>
</div>

<div class="card">
  <h2>Ручной джог</h2>
  <div class="grid2">
    <div><label>Pan</label><input type="range" id="jPan" min="0" max="180" oninput="jog()"></div>
    <div><label>Tilt</label><input type="range" id="jTilt" min="0" max="90" oninput="jog()"></div>
  </div>
</div>

<div class="card">
  <h2>Карты RSSI</h2>
  <div class="row">
    <button id="layBle" onclick="setLayer('ble')">Слой: BLE</button>
    <button id="layWifi" onclick="setLayer('wifi')">Слой: Wi-Fi</button>
  </div>
  <p class="small">Теплокарта: X = pan, Y = tilt. Серое = не измерено.</p>
  <canvas id="heat" width="360" height="200"></canvas>
  <p class="small">Полярный радар: направление максимума по азимуту (радиус = сила сигнала, по максимуму среди измеренных tilt). Нос антенны = 0°.</p>
  <canvas id="radar" width="360" height="220"></canvas>
  <div class="legend" id="legend"></div>
  <div class="row small" style="justify-content:space-between">
    <span>-95 dBm</span><span>-50 dBm</span>
  </div>
  <p><a class="dl" href="/api/map.csv" download="map.csv">Скачать карту CSV (pan,tilt,rssi_ble,rssi_wifi,t_ms)</a></p>
</div>

<div class="card">
  <h2>Калибровка</h2>
  <div class="grid2">
    <div><label>Pan мин, мкс</label><input id="cPanMinUs"></div>
    <div><label>Pan макс, мкс</label><input id="cPanMaxUs"></div>
    <div><label>Tilt мин, мкс</label><input id="cTiltMinUs"></div>
    <div><label>Tilt макс, мкс</label><input id="cTiltMaxUs"></div>
    <div><label>Pan угол мин°</label><input id="cPanAngMin"></div>
    <div><label>Pan угол макс°</label><input id="cPanAngMax"></div>
    <div><label>Tilt угол мин°</label><input id="cTiltAngMin"></div>
    <div><label>Tilt угол макс°</label><input id="cTiltAngMax"></div>
    <div><label>Порог RSSI, dBm</label><input id="cFloor"></div>
  </div>
  <div class="row" style="margin-top:8px">
    <button onclick="loadCal()">Обновить</button>
    <button class="primary" onclick="saveCal()">Сохранить в NVS</button>
  </div>
  <p class="small">Меняй мин/макс мкс, только глядя на серво живьём. Не выкручивай
  за реальный механический ход — сорвёшь шестерни.</p>
</div>

<script>
let layer='ble';
let mode='ble';

function q(id){return document.getElementById(id);}

async function cmd(action){
  // action вида 'park' / 'stop' / 'start&what=coarse' -> /api/park, /api/stop, /api/start?what=coarse
  await fetch('/api/'+action.replace('&','?'));
  refreshStatus();
}
async function setMode(m){
  mode=m;
  await fetch('/api/mode?mode='+m);
  updateModeButtons();
}
function updateModeButtons(){
  q('mBle').className = mode==='ble' ? 'primary' : '';
  q('mWifi').className = mode==='wifi' ? 'primary' : '';
}
function setLayer(l){
  layer=l;
  q('layBle').className = layer==='ble' ? 'primary' : '';
  q('layWifi').className = layer==='wifi' ? 'primary' : '';
  drawMaps();
}

// Throttle (не debounce!): шлём не чаще раза в 60мс, но не ждём остановки
// движения — firmware всё равно кладёт в очередь только самую свежую цель
// (см. servoTask()/qJog в .ino), так что здесь важно не заливать сеть
// запросами быстрее, чем это реально нужно джойстику, а не защищать плату.
const JOG_THROTTLE_MS = 60;
let jogLast = 0, jogTimer = null, jogPending = false;
function jogSend(){
  jogLast = performance.now();
  jogPending = false;
  fetch('/api/manual?pan='+q('jPan').value+'&tilt='+q('jTilt').value);
}
function jog(){
  const now = performance.now();
  const elapsed = now - jogLast;
  if (elapsed >= JOG_THROTTLE_MS) {
    clearTimeout(jogTimer);
    jogSend();
  } else if (!jogPending) {
    jogPending = true;
    jogTimer = setTimeout(jogSend, JOG_THROTTLE_MS - elapsed);
  }
}

async function refreshStatus(){
  try{
    const r=await fetch('/api/status'); const s=await r.json();
    q('pState').textContent='state: '+s.state;
    q('pMode').textContent='режим: '+s.mode;
    q('vPan').textContent=s.pan+'°';
    q('vTilt').textContent=s.tilt+'°';
    q('vRssiBle').textContent=(s.rssi_ble>-128? s.rssi_ble+' dBm':'—');
    q('vRssiWifi').textContent=(s.rssi_wifi>-128? s.rssi_wifi+' dBm':'—');
    q('tBle').textContent = s.target_ble_mac ? (s.target_ble_name||'(без имени)')+' · '+s.target_ble_mac : 'не выбрана';
    q('tWifi').textContent = s.target_wifi_bssid ? (s.target_wifi_ssid||'(скрыт)')+' · '+s.target_wifi_bssid+' ch'+s.target_wifi_ch : 'не выбрана';
    if(!q('jPan').matches(':active')) q('jPan').value=s.pan;
    if(!q('jTilt').matches(':active')) q('jTilt').value=s.tilt;
  }catch(e){}
}

async function refreshLists(){
  try{
    const rb=await fetch('/api/ble/list'); const bl=await rb.json();
    let tb=q('bleTable').tBodies[0]; tb.innerHTML='';
    bl.forEach(d=>{
      const tr=document.createElement('tr');
      const hasBest = d.best_pan!==undefined && d.best_pan>=0;
      const bestCell = hasBest ? `${d.best_pan}°/${d.best_tilt}° (${d.best_rssi}dBm)` : '—';
      const gotoBtn = hasBest ? `<button onclick="goBest(${d.best_pan},${d.best_tilt})">туда</button>` : '';
      tr.innerHTML=`<td>${d.name||'—'}</td><td>${d.mac}</td><td>${d.rssi}</td><td>${bestCell}</td>
        <td><button onclick="selBle('${d.mac}','${(d.name||'').replace(/'/g,"")}')">выбрать</button> ${gotoBtn}</td>`;
      tb.appendChild(tr);
    });
  }catch(e){}
  try{
    const rw=await fetch('/api/wifi/list'); const wl=await rw.json();
    let tw=q('wifiTable').tBodies[0]; tw.innerHTML='';
    wl.forEach(d=>{
      const tr=document.createElement('tr');
      tr.innerHTML=`<td>${d.ssid||'(hidden)'}</td><td>${d.bssid}</td><td>${d.channel}</td><td>${d.rssi}</td>
        <td><button onclick="selWifi('${d.bssid}',${d.channel},'${(d.ssid||'').replace(/'/g,"")}')">выбрать</button></td>`;
      tw.appendChild(tr);
    });
  }catch(e){}
}

function selBle(mac,name){ fetch('/api/select?type=ble&mac='+encodeURIComponent(mac)+'&name='+encodeURIComponent(name)).then(refreshStatus); }
// "Туда": сразу довернуть антенну на запомненный лучший угол для этого
// устройства (обычный ручной джог на конкретные координаты), не выбирая
// его целью и не запуская автонаведение.
function goBest(pan,tilt){
  q('jPan').value=pan; q('jTilt').value=tilt;
  fetch('/api/manual?pan='+pan+'&tilt='+tilt).then(refreshStatus);
}
function selWifi(bssid,ch,ssid){ fetch('/api/select?type=wifi&bssid='+encodeURIComponent(bssid)+'&channel='+ch+'&ssid='+encodeURIComponent(ssid)).then(refreshStatus); }

async function wifiFullscan(){
  q('wifiScanStatus').textContent='Скан 1..13 каналов, антенна должна быть в парковке...';
  await fetch('/api/wifi/fullscan');
  pollFullscan();
}
async function pollFullscan(){
  const r=await fetch('/api/wifi/fullscan/status'); const s=await r.json();
  if(s.busy){ q('wifiScanStatus').textContent='Идёт скан...'; setTimeout(pollFullscan,700); }
  else { q('wifiScanStatus').textContent='Готово.'; refreshLists(); }
}

// ---- карты ----
function rssiColor(v){
  if(v<=-128) return '#2a2f3a'; // не измерено
  const floor=-95, top=-40;
  let t=(v-floor)/(top-floor); t=Math.max(0,Math.min(1,t));
  // синий(слабо) -> зелёный -> красный(сильно)
  const r=Math.round(255*Math.min(1,t*2));
  const g=Math.round(255*Math.min(1,(1-Math.abs(t-0.5)*2)+0.15));
  const b=Math.round(255*Math.min(1,(1-t)*2));
  return `rgb(${r},${Math.max(0,Math.min(255,g))},${b})`;
}

let mapData=[]; // {pan,tilt,ble,wifi}
async function fetchMap(){
  try{
    const r=await fetch('/api/map.csv'); const txt=await r.text();
    const lines=txt.trim().split('\n');
    mapData=[];
    for(let i=1;i<lines.length;i++){
      const p=lines[i].split(',');
      if(p.length<4) continue;
      mapData.push({pan:+p[0],tilt:+p[1],ble:+p[2],wifi:+p[3]});
    }
    drawMaps();
  }catch(e){}
}

function drawMaps(){
  drawHeat();
  drawRadar();
}
function drawHeat(){
  const c=q('heat'); const ctx=c.getContext('2d');
  ctx.clearRect(0,0,c.width,c.height);
  if(mapData.length===0) return;
  let panMin=180,panMax=0,tiltMin=90,tiltMax=0;
  mapData.forEach(d=>{panMin=Math.min(panMin,d.pan);panMax=Math.max(panMax,d.pan);
                      tiltMin=Math.min(tiltMin,d.tilt);tiltMax=Math.max(tiltMax,d.tilt);});
  const pw=Math.max(1,panMax-panMin), th=Math.max(1,tiltMax-tiltMin);
  const cw=c.width/(pw+1), ch=c.height/(th+1);
  mapData.forEach(d=>{
    const v = layer==='ble' ? d.ble : d.wifi;
    ctx.fillStyle=rssiColor(v);
    const x=(d.pan-panMin)*cw, y=c.height-(d.tilt-tiltMin+1)*ch;
    ctx.fillRect(x,y,Math.ceil(cw),Math.ceil(ch));
  });
}
function drawRadar(){
  const c=q('radar'); const ctx=c.getContext('2d');
  ctx.clearRect(0,0,c.width,c.height);
  const cx=c.width/2, cy=c.height-10, R=Math.min(c.width/2-10,c.height-20);
  // сетка полукруга
  ctx.strokeStyle='#2a2f3a'; ctx.fillStyle='#8a93a6'; ctx.font='10px sans-serif';
  for(let rr=1;rr<=3;rr++){
    ctx.beginPath(); ctx.arc(cx,cy,R*rr/3,Math.PI,2*Math.PI); ctx.stroke();
  }
  ctx.beginPath(); ctx.moveTo(cx-R,cy); ctx.lineTo(cx+R,cy); ctx.stroke();
  ctx.fillText('0°',cx-R-2,cy+12); ctx.fillText('180°',cx+R-14,cy+12); ctx.fillText('90° (нос)',cx-16,cy-R-2);

  // по каждому pan берём максимум по всем измеренным tilt
  const byPan={};
  mapData.forEach(d=>{
    const v = layer==='ble' ? d.ble : d.wifi;
    if(v<=-128) return;
    if(!(d.pan in byPan) || v>byPan[d.pan]) byPan[d.pan]=v;
  });
  const floor=-95, top=-40;
  ctx.strokeStyle='#4fb0ff'; ctx.beginPath();
  let first=true, best=null;
  Object.keys(byPan).map(Number).sort((a,b)=>a-b).forEach(pan=>{
    const v=byPan[pan];
    let t=(v-floor)/(top-floor); t=Math.max(0.05,Math.min(1,t));
    const ang=Math.PI - (pan/180)*Math.PI; // 0°..180° -> полукруг слева направо
    const x=cx+Math.cos(ang)*R*t, y=cy-Math.sin(ang)*R*t;
    if(first){ctx.moveTo(x,y); first=false;} else ctx.lineTo(x,y);
    if(best===null || v>byPan[best]) best=pan;
  });
  ctx.stroke();
  if(best!==null){
    const v=byPan[best]; let t=(v-floor)/(top-floor); t=Math.max(0.05,Math.min(1,t));
    const ang=Math.PI-(best/180)*Math.PI;
    const x=cx+Math.cos(ang)*R*t, y=cy-Math.sin(ang)*R*t;
    ctx.fillStyle='#ff5f5f'; ctx.beginPath(); ctx.arc(x,y,4,0,7); ctx.fill();
    ctx.fillStyle='#e7ebf3'; ctx.fillText('пик: pan '+best+'° ('+v+' dBm)', 6, 12);
  }
}

function buildLegend(){
  const el=q('legend'); el.innerHTML='';
  for(let i=0;i<20;i++){
    const v=-95+i*(55/19);
    const d=document.createElement('div'); d.style.background=rssiColor(v); el.appendChild(d);
  }
}

async function loadCal(){
  const r=await fetch('/api/calibrate'); const c=await r.json();
  q('cPanMinUs').value=c.pan_min_us; q('cPanMaxUs').value=c.pan_max_us;
  q('cTiltMinUs').value=c.tilt_min_us; q('cTiltMaxUs').value=c.tilt_max_us;
  q('cPanAngMin').value=c.pan_angle_min; q('cPanAngMax').value=c.pan_angle_max;
  q('cTiltAngMin').value=c.tilt_angle_min; q('cTiltAngMax').value=c.tilt_angle_max;
  q('cFloor').value=c.rssi_floor;
}
async function saveCal(){
  const p=new URLSearchParams({
    pan_min_us:q('cPanMinUs').value, pan_max_us:q('cPanMaxUs').value,
    tilt_min_us:q('cTiltMinUs').value, tilt_max_us:q('cTiltMaxUs').value,
    pan_angle_min:q('cPanAngMin').value, pan_angle_max:q('cPanAngMax').value,
    tilt_angle_min:q('cTiltAngMin').value, tilt_angle_max:q('cTiltAngMax').value,
    rssi_floor:q('cFloor').value
  });
  await fetch('/api/calibrate/set?'+p.toString());
  alert('Сохранено в NVS');
}

buildLegend();
updateModeButtons();
loadCal();
refreshStatus(); refreshLists(); fetchMap();
setInterval(refreshStatus, 700);
setInterval(refreshLists, 3000);
setInterval(fetchMap, 1500);
</script>
</body>
</html>
)HTMLPAGE";
