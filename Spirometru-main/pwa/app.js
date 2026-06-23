'use strict';

// ═══════════════════════════════════════════════════════════════════
// BLE — identic cu firmware v12
// ═══════════════════════════════════════════════════════════════════
const BLE_NAME    = 'Spirometru';
const SVC_UUID    = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
const CHAR_UUID   = 'beb5483e-36e1-4688-b7f5-ea07361b26a8';
const CMD_UUID    = '2c7c3e4a-19b8-4f5a-9f45-c8e4d2b3a1f6';

// ═══════════════════════════════════════════════════════════════════
// FORMULE MEDICALE
// ═══════════════════════════════════════════════════════════════════

function predictedPef(age, h, sex) {
  age = +age; h = +h;
  return sex === 'M'
    ? Math.max(100, 3.95*h - 2.60*age - 22.9)
    : Math.max(80,  3.25*h - 1.85*age - 30.0);
}
function predictedFev1(age, h, sex) {
  age = +age; h = +h;
  return sex === 'M'
    ? Math.max(1,   0.0395*h - 0.025*age - 2.6)
    : Math.max(0.8, 0.032*h  - 0.020*age - 1.8);
}
function predictedFvc(age, h, sex) {
  age = +age; h = +h;
  return sex === 'M'
    ? Math.max(1.5, 0.0600*h - 0.028*age - 4.2)
    : Math.max(1.2, 0.0490*h - 0.022*age - 3.6);
}

function ginaZone(pef, predPef) {
  if (!predPef || predPef <= 0) return null;
  const pct = (pef / predPef) * 100;
  if (pct >= 80) return { zone:'green',  pct, label:`✓ Normal — ${pct.toFixed(0)}% din valoarea prezisă` };
  if (pct >= 60) return { zone:'yellow', pct, label:`⚠ Moderat — ${pct.toFixed(0)}% din valoarea prezisă` };
  return               { zone:'red',    pct, label:`✕ Sever — ${pct.toFixed(0)}% din valoarea prezisă` };
}

function tiffZone(it) {
  if (it <= 0) return null;
  if (it >= 70) return { zone:'green',  label:`IT ${it.toFixed(0)}% — Normal (≥70%)` };
  if (it >= 60) return { zone:'yellow', label:`IT ${it.toFixed(0)}% — Obstrucție ușoară` };
  return               { zone:'red',    label:`IT ${it.toFixed(0)}% — Obstrucție severă` };
}

// ═══════════════════════════════════════════════════════════════════
// STORAGE
// ═══════════════════════════════════════════════════════════════════
const RETENTION = 10 * 24 * 3600 * 1000;

const DB = {
  _l() { try { return JSON.parse(localStorage.getItem('spirometru')||'{}'); } catch { return {}; } },
  _s(db) { localStorage.setItem('spirometru', JSON.stringify(db)); },
  getPatients()      { return this._l().patients || []; },
  savePatient(p)     { const db=this._l(); if(!db.patients)db.patients=[]; const i=db.patients.findIndex(x=>x.id===p.id); if(i>=0)db.patients[i]=p; else db.patients.push(p); this._s(db); },
  deletePatient(id)  { const db=this._l(); db.patients=(db.patients||[]).filter(p=>p.id!==id); if(db.measurements)delete db.measurements[id]; this._s(db); },
  getMeasurements(pid){ const db=this._l(); const cut=Date.now()-RETENTION; return ((db.measurements||{})[pid]||[]).filter(m=>m.ts>cut); },
  saveMeasurement(pid,m){ const db=this._l(); if(!db.measurements)db.measurements={}; if(!db.measurements[pid])db.measurements[pid]=[]; db.measurements[pid].push(m); const cut=Date.now()-RETENTION; db.measurements[pid]=db.measurements[pid].filter(x=>x.ts>cut); this._s(db); },
  deleteMeasurement(pid,ts){ const db=this._l(); if(db.measurements?.[pid])db.measurements[pid]=db.measurements[pid].filter(m=>m.ts!==ts); this._s(db); },
  cleanup(){ const db=this._l(); if(!db.measurements)return; const cut=Date.now()-RETENTION; for(const pid of Object.keys(db.measurements))db.measurements[pid]=db.measurements[pid].filter(m=>m.ts>cut); this._s(db); }
};

// ═══════════════════════════════════════════════════════════════════
// BLE MANAGER — protocol firmware v12
// ═══════════════════════════════════════════════════════════════════
const BLE = {
  device:null, server:null, notify:null, cmd:null, connected:false,
  _inMeas:false, _raw:[], _rate:200,
  _res:{},  // rezultat curent
  onRaw:null, onResult:null, onStatus:null,

  async connect() {
    if (!navigator.bluetooth) { alert('Web Bluetooth nu e suportat.\nFolosește Chrome pe Android.'); return false; }
    try {
      this._setStatus('scanning');
      this.device = await navigator.bluetooth.requestDevice({ filters:[{name:BLE_NAME}], optionalServices:[SVC_UUID] });
      this.device.addEventListener('gattserverdisconnected', ()=>this._onDisconnect());
      this._setStatus('connecting');
      this.server = await this.device.gatt.connect();
      const svc   = await this.server.getPrimaryService(SVC_UUID);
      this.notify = await svc.getCharacteristic(CHAR_UUID);
      this.cmd    = await svc.getCharacteristic(CMD_UUID);
      await this.notify.startNotifications();
      this.notify.addEventListener('characteristicvaluechanged', e=>this._onData(e));
      this.connected = true; this._setStatus('connected'); return true;
    } catch(e) { console.error('BLE:',e); this._setStatus('disconnected'); return false; }
  },

  disconnect() { if(this.device?.gatt.connected)this.device.gatt.disconnect(); this.connected=false; this._setStatus('disconnected'); },

  async sendCmd(cmd) {
    if(!this.cmd||!this.connected)return;
    try { await this.cmd.writeValueWithoutResponse(new TextEncoder().encode(cmd)); } catch(e){console.error('CMD:',e);}
  },

  _onData(event) {
    const txt = new TextDecoder().decode(event.target.value).trim();

    if (txt.startsWith('MEAS_START')) {
      this._inMeas = true; this._raw = []; this._res = {};
      const m = txt.match(/RATE=(\d+)/); this._rate = m ? +m[1] : 200; return;
    }
    if (txt === 'MEAS_END') {
      this._inMeas = false;
      if (this.onResult) this.onResult({ ...this._res, samples: [...this._raw], rate: this._rate });
      return;
    }
    if (txt.startsWith('R:')) {
      const v = parseInt(txt.slice(2)); if(isNaN(v))return;
      if (this._inMeas) this._raw.push(v);
      else if (this.onRaw) this.onRaw(v);
      return;
    }

    // Parsare parametri
    const pairs = {
      'PEF:' :'pef',  'FEV1:':'fev1', 'FEVT:':'fevTotal',
      'PIF1:':'pif',  'FIV1:':'fiv1', 'VIR:' :'vir',
      'FVC:' :'fvc',  'IT:'  :'it'
    };
    for (const [prefix, key] of Object.entries(pairs)) {
      if (txt.startsWith(prefix)) { this._res[key] = parseFloat(txt.slice(prefix.length)); return; }
    }
  },

  _onDisconnect() { this.connected=false; this._inMeas=false; this._setStatus('disconnected'); },
  _setStatus(s) { if(this.onStatus)this.onStatus(s); }
};

// ═══════════════════════════════════════════════════════════════════
// GRAFICE
// ═══════════════════════════════════════════════════════════════════
function makeCanvas(el, dpr) {
  dpr = dpr || devicePixelRatio || 1;
  const r = el.getBoundingClientRect();
  el.width = r.width*dpr; el.height = r.height*dpr;
  const ctx = el.getContext('2d'); ctx.scale(dpr,dpr);
  return ctx;
}

const RealtimeChart = {
  el:null, ctx:null, buf:[], MAX:200*10,
  init(el){ this.el=el; this.ctx=makeCanvas(el); this.buf=[]; },
  push(v){ this.buf.push(v); if(this.buf.length>this.MAX)this.buf.shift(); },
  clear(){ this.buf=[]; this.draw(); },
  draw(){
    const ctx=this.ctx, dpr=devicePixelRatio||1;
    const W=this.el.width/dpr, H=this.el.height/dpr;
    ctx.clearRect(0,0,W,H);
    if(this.buf.length<2)return;
    const maxV=Math.max(50,...this.buf);
    ctx.strokeStyle='#ff9500'; ctx.lineWidth=1.5; ctx.beginPath();
    const step=W/Math.min(this.buf.length,this.MAX);
    const start=Math.max(0,this.buf.length-Math.floor(W/step));
    for(let i=start;i<this.buf.length;i++){
      const x=(i-start)*step, y=H-(this.buf[i]/maxV)*(H-6)-3;
      i===start?ctx.moveTo(x,y):ctx.lineTo(x,y);
    }
    ctx.stroke();
  }
};

const MeasureChart = {
  el:null, ctx:null,
  init(el){ this.el=el; this.ctx=makeCanvas(el); },
  draw(samples){
    const ctx=this.ctx, dpr=devicePixelRatio||1;
    const W=this.el.width/dpr, H=this.el.height/dpr;
    ctx.clearRect(0,0,W,H);
    if(!samples||samples.length<2)return;
    const maxV=Math.max(...samples), minV=Math.min(...samples);
    // Linie zero (ref)
    const refY = H - ((minV+Math.round((maxV-minV)*0.3)-minV)/(maxV-minV||1))*(H-6)-3;
    ctx.strokeStyle='rgba(255,255,255,0.15)'; ctx.lineWidth=1;
    ctx.setLineDash([4,4]); ctx.beginPath(); ctx.moveTo(0,H/2); ctx.lineTo(W,H/2); ctx.stroke();
    ctx.setLineDash([]);
    // Semnal
    ctx.strokeStyle='#ff9500'; ctx.lineWidth=1.5; ctx.beginPath();
    for(let i=0;i<samples.length;i++){
      const x=(i/(samples.length-1))*W, y=H-((samples[i]-minV)/(maxV-minV||1))*(H-6)-3;
      i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
    }
    ctx.stroke();
  }
};

const HistoryChart = {
  el:null, ctx:null,
  init(el){ this.el=el; this.ctx=makeCanvas(el); },
  draw(ms, metric, predicted){
    const ctx=this.ctx, dpr=devicePixelRatio||1;
    const W=this.el.width/dpr, H=this.el.height/dpr;
    ctx.clearRect(0,0,W,H);
    if(!ms?.length){ ctx.fillStyle='#3a3a5a'; ctx.font='13px sans-serif'; ctx.textAlign='center'; ctx.fillText('Nicio măsurătoare',W/2,H/2); return; }
    const vals=ms.map(m=>metric==='pef'?m.pef:metric==='fev1'?m.fev1:metric==='fvc'?m.fvc:m.it||0);
    const minV=Math.max(0,Math.min(...vals)*0.88), maxV=Math.max(...vals)*1.12;
    const color=metric==='pef'?'#ff9500':metric==='fev1'?'#00c8ff':metric==='fvc'?'#00e676':'#ffcc00';
    const PL=44,PR=16,PT=16,PB=30, gW=W-PL-PR, gH=H-PT-PB;
    const toX=i=>PL+(i/Math.max(vals.length-1,1))*gW;
    const toY=v=>PT+gH-((v-minV)/(maxV-minV||1))*gH;
    for(let i=0;i<=4;i++){
      const y=PT+(gH/4)*i; ctx.strokeStyle='#1a1a2e'; ctx.lineWidth=1;
      ctx.beginPath(); ctx.moveTo(PL,y); ctx.lineTo(W-PR,y); ctx.stroke();
      const v=maxV-(maxV-minV)*i/4; ctx.fillStyle='#3a3a5a'; ctx.font='10px sans-serif'; ctx.textAlign='right';
      ctx.fillText(v.toFixed(metric==='pef'||metric==='it'?0:1),PL-4,y+4);
    }
    if(predicted>0&&metric!=='it'){
      const lim=predicted*0.8;
      if(lim>=minV&&lim<=maxV){
        const yL=toY(lim); ctx.strokeStyle='rgba(255,68,68,0.4)'; ctx.lineWidth=1;
        ctx.setLineDash([4,4]); ctx.beginPath(); ctx.moveTo(PL,yL); ctx.lineTo(W-PR,yL); ctx.stroke(); ctx.setLineDash([]);
        ctx.fillStyle='rgba(255,68,68,0.6)'; ctx.font='10px sans-serif'; ctx.textAlign='left'; ctx.fillText('80%',W-PR-28,yL-3);
      }
    }
    // Linie 70% IT
    if(metric==='it'&&70>=minV&&70<=maxV){
      const yL=toY(70); ctx.strokeStyle='rgba(255,68,68,0.4)'; ctx.lineWidth=1;
      ctx.setLineDash([4,4]); ctx.beginPath(); ctx.moveTo(PL,yL); ctx.lineTo(W-PR,yL); ctx.stroke(); ctx.setLineDash([]);
    }
    ctx.beginPath(); ctx.moveTo(toX(0),toY(vals[0]));
    for(let i=1;i<vals.length;i++) ctx.lineTo(toX(i),toY(vals[i]));
    ctx.lineTo(toX(vals.length-1),H-PB); ctx.lineTo(toX(0),H-PB); ctx.closePath();
    ctx.fillStyle=color+'18'; ctx.fill();
    ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath(); ctx.moveTo(toX(0),toY(vals[0]));
    for(let i=1;i<vals.length;i++) ctx.lineTo(toX(i),toY(vals[i])); ctx.stroke();
    vals.forEach((v,i)=>{ ctx.fillStyle=color; ctx.beginPath(); ctx.arc(toX(i),toY(v),4,0,Math.PI*2); ctx.fill(); });
    ctx.fillStyle='#3a3a5a'; ctx.font='10px sans-serif'; ctx.textAlign='center';
    const step=Math.max(1,Math.floor(ms.length/5));
    for(let i=0;i<ms.length;i+=step){ const d=new Date(ms[i].ts); ctx.fillText(`${d.getDate()}/${d.getMonth()+1}`,toX(i),H-PB+14); }
  }
};

// ═══════════════════════════════════════════════════════════════════
// APP
// ═══════════════════════════════════════════════════════════════════
const App = {
  currentPatientId: null,
  currentPatient:   null,
  chartMetric:      'pef',
  pendingResult:    null,
  waitingMeasure:   false,

  init() {
    DB.cleanup();
    this.renderPatientList();
    BLE.onStatus = s => this.onBleStatus(s);
    BLE.onRaw    = v => RealtimeChart.push(v);
    BLE.onResult = r => this.onResult(r);

    RealtimeChart.init(document.getElementById('realtime-canvas'));
    MeasureChart.init(document.getElementById('measure-canvas'));
    HistoryChart.init(document.getElementById('history-canvas'));

    const loop = () => { if(BLE.connected) RealtimeChart.draw(); requestAnimationFrame(loop); };
    requestAnimationFrame(loop);
    if('serviceWorker'in navigator) navigator.serviceWorker.register('sw.js').catch(()=>{});
  },

  async connectBle() { if(BLE.connected){BLE.disconnect();return;} await BLE.connect(); },

  onBleStatus(s) {
    const btn=document.getElementById('btn-connect');
    const label=document.getElementById('connect-label');
    const map={disconnected:'Conectează',scanning:'Scanează…',connecting:'Conectare…',connected:'Deconectează'};
    label.textContent=map[s]||s;
    btn.className='btn-connect'+(s==='connected'?' connected':s==='scanning'||s==='connecting'?' scanning':'');
  },

  onResult(r) {
    this.pendingResult = { ...r, ts: Date.now() };
    MeasureChart.draw(r.samples);
    RealtimeChart.clear();

    // Afiseaza toti parametrii
    this._setMetric('live-pef',  r.pef,      0);
    this._setMetric('live-fev1', r.fev1,     2);
    this._setMetric('live-fevt', r.fevTotal, 2);
    this._setMetric('live-pif',  r.pif,      0);
    this._setMetric('live-fiv1', r.fiv1,     2);
    this._setMetric('live-vir',  r.vir,      2);
    this._setMetric('live-fvc',  r.fvc,      2);
    this._setMetric('live-it',   r.it,       0, '%');

    // Zona GINA
    if (this.currentPatient) {
      const p = this.currentPatient;
      const predPef = predictedPef(p.age, p.height, p.sex);
      const zone = ginaZone(r.pef, predPef);
      if (zone) {
        const el = document.getElementById('gina-zone');
        el.className = `gina-zone gina-${zone.zone}`;
        el.textContent = zone.label;
      }
      // Zona Tiffeneau
      if (r.it > 0) {
        const zIT = tiffZone(r.it);
        if (zIT) {
          const el2 = document.getElementById('tiff-zone');
          el2.className = `gina-zone gina-${zIT.zone}`;
          el2.textContent = zIT.label;
          el2.classList.remove('hidden');
        }
      }
    }

    document.getElementById('btn-save-result').classList.remove('hidden');
    document.getElementById('btn-measure').textContent = '▶ Start măsurătoare';
    document.getElementById('btn-measure').classList.remove('measuring');
    this.waitingMeasure = false;
    document.getElementById('realtime-canvas').classList.add('hidden');
    document.getElementById('measure-canvas').classList.remove('hidden');
  },

  _setMetric(id, val, dp, suffix='') {
    const el = document.getElementById(id);
    if (!el) return;
    el.textContent = (val != null && val > 0) ? val.toFixed(dp) + suffix : '---';
  },

  toggleMeasure() {
    if (!BLE.connected) { alert('Conectează spirometrul mai întâi!'); return; }
    if (this.waitingMeasure) {
      BLE.sendCmd('RST');
      this.waitingMeasure = false;
      document.getElementById('btn-measure').textContent = '▶ Start măsurătoare';
      document.getElementById('btn-measure').classList.remove('measuring');
      document.getElementById('realtime-canvas').classList.remove('hidden');
      document.getElementById('measure-canvas').classList.add('hidden');
      return;
    }
    this.waitingMeasure = true;
    this.pendingResult  = null;
    ['live-pef','live-fev1','live-fevt','live-pif','live-fiv1','live-vir','live-fvc','live-it']
      .forEach(id => { const el=document.getElementById(id); if(el)el.textContent='---'; });
    document.getElementById('gina-zone').className = 'gina-zone hidden';
    document.getElementById('tiff-zone').className = 'gina-zone hidden';
    document.getElementById('btn-save-result').classList.add('hidden');
    document.getElementById('btn-measure').textContent = '■ Anulează';
    document.getElementById('btn-measure').classList.add('measuring');
    document.getElementById('realtime-canvas').classList.remove('hidden');
    document.getElementById('measure-canvas').classList.add('hidden');
    RealtimeChart.clear();
  },

  saveResult() {
    if (!this.pendingResult || !this.currentPatientId) return;
    const m = {
      pef: this.pendingResult.pef   || 0,
      fev1:this.pendingResult.fev1  || 0,
      fevTotal:this.pendingResult.fevTotal||0,
      pif: this.pendingResult.pif   || 0,
      fiv1:this.pendingResult.fiv1  || 0,
      vir: this.pendingResult.vir   || 0,
      fvc: this.pendingResult.fvc   || 0,
      it:  this.pendingResult.it    || 0,
      ts:  this.pendingResult.ts
    };
    DB.saveMeasurement(this.currentPatientId, m);
    this.pendingResult = null;
    document.getElementById('btn-save-result').classList.add('hidden');
    this.renderHistory(); this.renderChart(); this.renderPatientList();
  },

  calibrate() {
    if(!BLE.connected){alert('Conectează spirometrul mai întâi!');return;}
    BLE.sendCmd('CAL');
  },

  // ── Navigare ─────────────────────────────────────────────────
  showScreen(id) { document.querySelectorAll('.screen').forEach(s=>s.classList.remove('active')); document.getElementById(id).classList.add('active'); },
  showPatients() { this.renderPatientList(); this.showScreen('screen-patients'); },

  showNewPatient() {
    document.getElementById('form-title').textContent='Pacient nou';
    ['id','name','age','height','weight','notes'].forEach(f=>document.getElementById('field-'+f).value='');
    document.getElementById('field-sex').value='M';
    this.showScreen('screen-patient-form');
  },

  editCurrentPatient() {
    const p=this.currentPatient; if(!p)return;
    document.getElementById('form-title').textContent='Editare pacient';
    document.getElementById('field-id').value=p.id;
    document.getElementById('field-name').value=p.name;
    document.getElementById('field-age').value=p.age;
    document.getElementById('field-sex').value=p.sex;
    document.getElementById('field-height').value=p.height;
    document.getElementById('field-weight').value=p.weight||'';
    document.getElementById('field-notes').value=p.notes||'';
    this.showScreen('screen-patient-form');
  },

  savePatient() {
    const name=document.getElementById('field-name').value.trim();
    const age=document.getElementById('field-age').value;
    const height=document.getElementById('field-height').value;
    if(!name||!age||!height){alert('Completează: Nume, Vârstă și Înălțime.');return;}
    DB.savePatient({ id:document.getElementById('field-id').value||Date.now().toString(), name, age, sex:document.getElementById('field-sex').value, height, weight:document.getElementById('field-weight').value, notes:document.getElementById('field-notes').value.trim() });
    this.showPatients();
  },

  openPatient(id) {
    const p=DB.getPatients().find(x=>x.id===id); if(!p)return;
    this.currentPatient=p; this.currentPatientId=id;
    document.getElementById('detail-name').textContent=p.name;
    const predPef=predictedPef(p.age,p.height,p.sex);
    const predFev1=predictedFev1(p.age,p.height,p.sex);
    const predFvc=predictedFvc(p.age,p.height,p.sex);
    document.getElementById('detail-info').textContent=
      `${p.sex==='M'?'♂':'♀'} ${p.age} ani · ${p.height} cm${p.weight?' · '+p.weight+' kg':''} · PEF: ${Math.round(predPef)} · FEV1: ${predFev1.toFixed(1)} · FVC: ${predFvc.toFixed(1)} L`;
    this.showTab('tab-measure');
    this.showScreen('screen-patient-detail');
    this.renderHistory(); this.renderChart();
    this.waitingMeasure=false; this.pendingResult=null;
    ['live-pef','live-fev1','live-fevt','live-pif','live-fiv1','live-vir','live-fvc','live-it']
      .forEach(id=>{const el=document.getElementById(id);if(el)el.textContent='---';});
    document.getElementById('gina-zone').className='gina-zone hidden';
    document.getElementById('tiff-zone').className='gina-zone hidden';
    document.getElementById('btn-save-result').classList.add('hidden');
    document.getElementById('btn-measure').textContent='▶ Start măsurătoare';
    document.getElementById('btn-measure').classList.remove('measuring');
    document.getElementById('realtime-canvas').classList.remove('hidden');
    document.getElementById('measure-canvas').classList.add('hidden');
    RealtimeChart.clear();
  },

  showTab(id) {
    document.querySelectorAll('.tab-content').forEach(t=>t.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
    document.getElementById(id).classList.add('active');
    const idx=['tab-measure','tab-history','tab-chart'].indexOf(id);
    document.querySelectorAll('.tab')[idx]?.classList.add('active');
    if(id==='tab-chart') setTimeout(()=>this.renderChart(),50);
  },

  setChartMetric(metric, btn) {
    this.chartMetric=metric;
    document.querySelectorAll('.chip').forEach(c=>c.classList.remove('active'));
    btn.classList.add('active');
    this.renderChart();
  },

  // ── Render ───────────────────────────────────────────────────
  renderPatientList(filter) {
    let pts=DB.getPatients();
    if(filter){const f=filter.toLowerCase();pts=pts.filter(p=>p.name.toLowerCase().includes(f));}
    const list=document.getElementById('patient-list');
    if(!pts.length){list.innerHTML=`<div class="empty-state"><div class="empty-icon">🫁</div><div>${filter?'Niciun pacient găsit.':'Niciun pacient înregistrat.<br>Apasă <b>+ Pacient nou</b>.'}</div></div>`;return;}
    list.innerHTML=pts.map(p=>{
      const ms=DB.getMeasurements(p.id); const last=ms[ms.length-1];
      return `<div class="patient-item" onclick="App.openPatient('${p.id}')">
        <div class="patient-avatar">${p.sex==='M'?'👨':'👩'}</div>
        <div class="patient-info">
          <div class="patient-name">${p.name}</div>
          <div class="patient-meta">${p.sex==='M'?'♂':'♀'} ${p.age} ani · ${p.height} cm · ${ms.length} măsurători</div>
        </div>
        <div class="patient-last-pef">${last?`<span style="color:var(--pef)">${last.pef} L/min</span>`:''}</div>
        <button class="patient-item-delete" onclick="event.stopPropagation();App.confirmDeletePatient('${p.id}','${p.name.replace(/'/g,"\\'")}')">🗑</button>
      </div>`;
    }).join('');
  },

  searchPatients(v) { this.renderPatientList(v); },

  renderHistory() {
    if(!this.currentPatientId)return;
    const ms=DB.getMeasurements(this.currentPatientId);
    const list=document.getElementById('history-list');
    if(!ms.length){list.innerHTML='<div class="empty-state"><div class="empty-icon">📋</div><div>Nicio măsurătoare încă.</div></div>';return;}
    const predPef=predictedPef(this.currentPatient.age,this.currentPatient.height,this.currentPatient.sex);
    list.innerHTML=[...ms].reverse().map(m=>{
      const zone=ginaZone(m.pef,predPef);
      const dot=zone?`var(--${zone.zone==='green'?'green':zone.zone==='yellow'?'yellow':'red'})`:'#444';
      const d=new Date(m.ts);
      const itStr=m.it>0?` · IT:${m.it.toFixed(0)}%`:'';
      const fvcStr=m.fvc>0?` · FVC:${m.fvc.toFixed(1)}L`:'';
      return `<div class="history-item">
        <div class="history-dot" style="background:${dot}"></div>
        <div class="history-data">
          <div class="history-values">
            <span class="pef-val">${m.pef} L/min</span>
            <span class="fev1-val">FEV1:${m.fev1}L</span>
            <span class="fev1-val">${fvcStr}${itStr}</span>
          </div>
          ${m.pif>0?`<div style="font-size:11px;color:var(--fev1);margin-top:2px">PIF:${m.pif} L/min · FIV1:${m.fiv1}L · VIR:${m.vir}L</div>`:''}
          <div class="history-date">${d.toLocaleDateString('ro-RO')} ${d.toLocaleTimeString('ro-RO',{hour:'2-digit',minute:'2-digit'})}</div>
        </div>
        <button class="history-delete" onclick="App.confirmDeleteMeasurement(${m.ts})">✕</button>
      </div>`;
    }).join('');
  },

  renderChart() {
    if(!this.currentPatientId)return;
    const ms=DB.getMeasurements(this.currentPatientId);
    const p=this.currentPatient;
    const predMap={pef:predictedPef(p.age,p.height,p.sex),fev1:predictedFev1(p.age,p.height,p.sex),fvc:predictedFvc(p.age,p.height,p.sex),it:70};
    const pred=predMap[this.chartMetric]||0;
    HistoryChart.draw(ms,this.chartMetric,pred);
    if(ms.length){
      const vals=ms.map(m=>this.chartMetric==='pef'?m.pef:this.chartMetric==='fev1'?m.fev1:this.chartMetric==='fvc'?m.fvc:m.it||0);
      const avg=vals.reduce((a,b)=>a+b,0)/vals.length;
      const dp=this.chartMetric==='pef'||this.chartMetric==='it'?0:1;
      const unit=this.chartMetric==='pef'?'L/min':this.chartMetric==='it'?'%':'L';
      document.getElementById('chart-stats').innerHTML=`
        <div class="stat-box"><div class="stat-val" style="color:var(--pef)">${Math.max(...vals).toFixed(dp)}</div><div class="stat-lbl">Max ${unit}</div></div>
        <div class="stat-box"><div class="stat-val">${avg.toFixed(dp)}</div><div class="stat-lbl">Medie ${unit}</div></div>
        <div class="stat-box"><div class="stat-val" style="color:var(--text-muted)">${Math.min(...vals).toFixed(dp)}</div><div class="stat-lbl">Min ${unit}</div></div>
        <div class="stat-box"><div class="stat-val" style="color:var(--accent)">${ms.length}</div><div class="stat-lbl">Total</div></div>`;
    } else { document.getElementById('chart-stats').innerHTML=''; }
  },

  confirmDeletePatient(id,name){
    document.getElementById('modal-text').textContent=`Ștergi pacientul "${name}" și toate măsurătorile lui?`;
    document.getElementById('modal-confirm').onclick=()=>{DB.deletePatient(id);this.closeModal();this.renderPatientList();};
    document.getElementById('modal-overlay').classList.remove('hidden');
  },
  confirmDeleteMeasurement(ts){
    document.getElementById('modal-text').textContent='Ștergi această măsurătoare?';
    document.getElementById('modal-confirm').onclick=()=>{DB.deleteMeasurement(this.currentPatientId,ts);this.closeModal();this.renderHistory();this.renderChart();this.renderPatientList();};
    document.getElementById('modal-overlay').classList.remove('hidden');
  },
  closeModal(){ document.getElementById('modal-overlay').classList.add('hidden'); }
};

document.addEventListener('DOMContentLoaded', ()=>App.init());
