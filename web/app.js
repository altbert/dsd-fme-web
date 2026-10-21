/* DSD-FME web client */
'use strict';
const $ = (id) => document.getElementById(id);
const api = {};          // populated on connect
let ws = null, token = '', needPassword = false, authed = false;
let lastFFT = null;      // {bins,center,rate,dbmin,vals}
let noiseFloor = -110;
let curFreq = 0, curMode = '', autoOn = false;   // from the last status

//---------------------------------------------------------------- websocket
function wsURL(){ const p = location.protocol === 'https:' ? 'wss' : 'ws'; return p + '://' + location.host + '/ws'; }

function connect(){
  ws = new WebSocket(wsURL());
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { setConn('connecting', 'off'); resync(); };
  ws.onclose = () => { setConn('offline','off'); authed=false; setTimeout(connect, 1500); };
  ws.onerror = () => {};
  ws.onmessage = onMessage;
}
function send(obj){ if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function subscribe(){ send({cmd:'subscribe', fft:true, audio:audioOn, console:true}); }
function resync(){ subscribe(); send({cmd:'get_status'}); send({cmd:'bm_list'}); }

function onMessage(ev){
  if (typeof ev.data === 'string'){
    if (ev.data[0] === '{'){ try { handleJSON(JSON.parse(ev.data)); } catch(e){} }
    else logConsole(ev.data);
    return;
  }
  const dv = new DataView(ev.data);
  const t = dv.getUint8(0);
  if (t === 1) handleFFT(dv, ev.data);
  else if (t === 2) handleAudio(dv, ev.data);
}

function handleJSON(m){
  if (m.type === 'hello'){
    token = m.token || ''; needPassword = !!m.password;
    if (needPassword && !authed){ $('login').classList.remove('hidden'); }
    else { authed = true; $('login').classList.add('hidden'); setConn('connected','live'); resync(); }
    if (m.rtl === false) $('tunefreq').placeholder = 'RIGCTL tune (SDR++ etc)';
  }
  else if (m.type === 'auth'){
    if (m.ok){ authed = true; $('login').classList.add('hidden'); $('pw-err').textContent=''; setConn('connected','live'); resync(); }
    else { $('pw-err').textContent = 'Wrong password'; }
  }
  else if (m.type === 'status'){ authed = true; setConn('connected','live'); renderStatus(m); }
  else if (m.type === 'files'){ renderFiles(m); }
  else if (m.type === 'bookmarks'){ bms = m.items || []; $('bm-path').textContent = m.path || '—'; renderBookmarks(); }
  else if (m.type === 'bm_saved'){ bmFormClose(); bmMsg(m.warn ? 'Saved, but '+m.warn+'.' : 'Saved.', !!m.warn); }
  else if (m.type === 'bm_error'){ bmMsg('Not saved: '+m.msg+'.', true); }
  else if (m.type === 'bm_imported'){ bmMsg(m.msg); }
}

function setConn(txt, cls){ const b=$('st-conn'); b.textContent=txt; b.className='badge '+cls; }

//---------------------------------------------------------------- spectrum
const spec = $('spec'), wf = $('wf');
const sctx = spec.getContext('2d'), wctx = wf.getContext('2d');
let wfImg = null;

function sizeCanvas(){
  for (const c of [spec, wf]){
    const w = c.clientWidth|0, h = c.clientHeight|0;
    if (c.width !== w || c.height !== h){ c.width = w; c.height = h; }
  }
  wfImg = wf.width > 0 ? wctx.createImageData(wf.width, 1) : null; // zero while the page has no layout width
}
window.addEventListener('resize', sizeCanvas);

function dbColor(n){ // n in 0..1
  n = Math.max(0, Math.min(1, n));
  // blue -> cyan -> green -> yellow -> red
  const stops = [[8,12,28],[20,60,120],[0,160,150],[90,210,70],[240,220,60],[255,80,40]];
  const x = n*(stops.length-1); const i = Math.floor(x), f = x-i;
  const a = stops[i], b = stops[Math.min(i+1,stops.length-1)];
  return [a[0]+(b[0]-a[0])*f, a[1]+(b[1]-a[1])*f, a[2]+(b[2]-a[2])*f];
}

function handleFFT(dv, buf){
  const bins = dv.getUint16(2, true);
  const center = dv.getUint32(4, true);
  const rate = dv.getUint32(8, true);
  const dbmin = dv.getFloat32(12, true);
  const u8 = new Uint8Array(buf, 16, bins);
  const vals = new Float32Array(bins);
  let mn = 999;
  for (let i=0;i<bins;i++){ const d = dbmin + u8[i]*0.5; vals[i]=d; if (d<mn) mn=d; }
  noiseFloor = noiseFloor*0.9 + mn*0.1;
  lastFFT = {bins, center, rate, dbmin, vals};
  drawSpectrum();
  $('specmeta').textContent = (center/1e6).toFixed(4)+' MHz · '+(rate/1e6).toFixed(2)+' Msps · '+bins;
}

function drawSpectrum(){
  if (!lastFFT || !spec.width || !wfImg || wfImg.width !== wf.width) return;
  const {bins, center, rate, vals} = lastFFT;
  const W = spec.width, H = spec.height;
  const range = +$('dbrange').value;
  const floor = noiseFloor - 3, top = floor + range;
  sctx.clearRect(0,0,W,H);
  // grid
  sctx.strokeStyle = 'rgba(255,255,255,0.05)'; sctx.lineWidth=1;
  for (let g=1;g<6;g++){ const x=W*g/6; sctx.beginPath(); sctx.moveTo(x,0); sctx.lineTo(x,H); sctx.stroke(); }
  // center line
  sctx.strokeStyle = 'rgba(90,169,255,0.35)'; sctx.beginPath(); sctx.moveTo(W/2,0); sctx.lineTo(W/2,H); sctx.stroke();
  // fill + line
  sctx.beginPath();
  for (let x=0;x<W;x++){
    const i = Math.min(bins-1, (x/W*bins)|0);
    const n = (vals[i]-floor)/(top-floor);
    const y = H - Math.max(0,Math.min(1,n))*H;
    if (x===0) sctx.moveTo(x,y); else sctx.lineTo(x,y);
  }
  sctx.lineTo(W,H); sctx.lineTo(0,H); sctx.closePath();
  sctx.fillStyle = 'rgba(53,208,165,0.12)'; sctx.fill();
  sctx.beginPath();
  for (let x=0;x<W;x++){
    const i = Math.min(bins-1, (x/W*bins)|0);
    const n = (vals[i]-floor)/(top-floor);
    const y = H - Math.max(0,Math.min(1,n))*H;
    if (x===0) sctx.moveTo(x,y); else sctx.lineTo(x,y);
  }
  sctx.strokeStyle = '#35d0a5'; sctx.lineWidth=1.2; sctx.stroke();
  drawMarks(center, rate, W, H);
  // waterfall row
  const Wf = wf.width;
  const img = wfImg;
  for (let x=0;x<Wf;x++){
    const i = Math.min(bins-1, (x/Wf*bins)|0);
    const n = (vals[i]-floor)/(top-floor);
    const c = dbColor(n);
    const o = x*4; img.data[o]=c[0]; img.data[o+1]=c[1]; img.data[o+2]=c[2]; img.data[o+3]=255;
  }
  wctx.drawImage(wf, 0, 0, Wf, wf.height-1, 0, 1, Wf, wf.height-1);
  wctx.putImageData(img, 0, 0);
}

// bookmark markers inside the visible span; labels step down a row when they would overlap
let marks = [];
function drawMarks(center, rate, W, H){
  marks = [];
  if (!bms.length || !rate) return;
  const lo = center - rate/2;
  const vis = bms.filter(b => b.freq >= lo && b.freq <= lo + rate).sort((a,b) => a.freq - b.freq);
  sctx.font = '10px ui-monospace,Menlo,Consolas,monospace'; sctx.textBaseline = 'top';
  const rowEnd = [-1e9, -1e9, -1e9, -1e9];
  for (const b of vis){
    const x = Math.round((b.freq - lo) / rate * W) + 0.5;
    sctx.strokeStyle = 'rgba(255,207,90,0.5)'; sctx.setLineDash([3,3]);
    sctx.beginPath(); sctx.moveTo(x, 0); sctx.lineTo(x, H); sctx.stroke(); sctx.setLineDash([]);
    const tw = Math.min(140, sctx.measureText(b.name).width + 6);
    const lx = Math.max(0, Math.min(W - tw, x + 2));
    let row = rowEnd.findIndex(e => lx > e + 2);
    if (row < 0) row = rowEnd.indexOf(Math.min(...rowEnd)); // all rows taken: the one that frees up first
    rowEnd[row] = lx + tw;
    const ly = 4 + row*13;
    sctx.fillStyle = 'rgba(5,8,12,0.78)'; sctx.fillRect(lx, ly, tw, 12);
    sctx.fillStyle = '#ffcf5a'; sctx.fillText(b.name, lx + 3, ly + 1, tw - 6);
    marks.push({x, lx, ly, tw, b});
  }
}
function markAt(ev){
  const r = spec.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
  for (const m of marks){
    if (ev.target === spec && x >= m.lx && x <= m.lx + m.tw && y >= m.ly && y <= m.ly + 12) return m;
  }
  for (const m of marks) if (Math.abs(x - m.x) <= 4) return m;
  return null;
}

function specToFreq(ev){
  if (!lastFFT) return null;
  const r = spec.getBoundingClientRect();
  const x = (ev.clientX - r.left)/r.width;
  return Math.round(lastFFT.center + (x-0.5)*lastFFT.rate);
}
for (const el of [spec, wf]){
  el.addEventListener('click', (ev)=>{ const m=markAt(ev); if (m){ bmTune(m.b); return; } const f=specToFreq(ev); if (f) tuneTo(f); });
  el.addEventListener('mousemove', (ev)=>{
    const f=specToFreq(ev); const cur=$('cursor'); if(!f){cur.style.display='none';return;}
    const r=$('specbox').getBoundingClientRect(); cur.style.display='block';
    cur.style.left=(ev.clientX-r.left)+'px';
    const m=markAt(ev);
    $('specinfo').textContent = m ? '★ '+m.b.name+' · '+fmtMHz(m.b.freq)+' MHz' : (f/1e6).toFixed(5)+' MHz';
  });
  el.addEventListener('mouseleave', ()=>{ $('cursor').style.display='none'; $('specinfo').textContent=''; });
}
function tuneTo(freq){ send({cmd:'tune', freq}); $('tunefreq').value=(freq/1e6).toFixed(6); }

//---------------------------------------------------------------- audio
let actx=null, gain=null, playTime=0, audioOn=false;
function audioStart(){
  if (!actx){ actx=new (window.AudioContext||window.webkitAudioContext)(); gain=actx.createGain(); gain.connect(actx.destination); gain.gain.value=+$('vol').value/100; }
  actx.resume(); audioOn=true; playTime=0;
  $('btn-audio').textContent='🔊 Audio on'; $('btn-audio').classList.add('p');
  subscribe();
}
function audioStop(){ audioOn=false; $('btn-audio').textContent='🔇 Audio off'; $('btn-audio').classList.remove('p'); subscribe(); }
$('btn-audio').onclick = ()=> audioOn ? audioStop() : audioStart();
$('vol').oninput = ()=>{ if (gain) gain.gain.value = +$('vol').value/100; };

function handleAudio(dv, buf){
  if (!audioOn || !actx) return;
  const ch = dv.getUint8(1), fmt = dv.getUint8(2), rate = dv.getUint16(3, true)*100;
  const body = buf.byteLength - 8;
  const bps = fmt===1 ? 4 : 2;
  const frames = (body / bps / ch)|0;
  if (frames<=0) return;
  const abuf = actx.createBuffer(ch, frames, rate);
  if (fmt===1){
    const f = new Float32Array(buf, 8, frames*ch);
    for (let c=0;c<ch;c++){ const cd=abuf.getChannelData(c); for (let i=0;i<frames;i++) cd[i]=f[i*ch+c]; }
  } else {
    const s = new Int16Array(buf, 8, frames*ch);
    for (let c=0;c<ch;c++){ const cd=abuf.getChannelData(c); for (let i=0;i<frames;i++) cd[i]=s[i*ch+c]/32768; }
  }
  const src = actx.createBufferSource(); src.buffer=abuf; src.connect(gain);
  const now = actx.currentTime;
  if (playTime < now + 0.04 || playTime > now + 0.6) playTime = now + 0.12; // resync jitter buffer
  src.start(playTime); playTime += frames/rate;
}

//---------------------------------------------------------------- status render
function esc(s){ return (s==null?'':(''+s)); }
// decoder strings (talker alias, call text, GPS) come over the air: escape before innerHTML
function escH(s){ return esc(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function renderStatus(m){
  const d=m.decode||{}, inp=m.input||{}, tr=m.trunk||{}, fl=m.flags||{}, site=m.site||{}, au=m.auto||{};
  const ana = !d.carrier && d.analog;
  $('st-carrier').textContent = d.carrier ? 'SYNC' : (ana ? 'ANALOG' : 'no sync');
  $('st-carrier').className = 'badge '+(d.carrier?'live':(ana?'warn':''));
  $('st-mode').textContent = esc(d.name);
  $('d-freq').textContent = (inp.freq/1e6).toFixed(6);
  $('d-sys').textContent = ana ? 'Analog FM' : esc(d.sys);
  $('d-ftype').textContent = esc(d.ftype).trim()||'—';
  $('d-mod').textContent = esc(d.mod)+' · '+esc(d.rate)+' sps';
  $('d-level').style.width = (d.level||0)+'%';
  const sq=inp.squelch||1, rms=inp.rms||0;
  $('d-rms').style.width = Math.min(100, rms/ (sq*2||200) *100)+'%';
  $('d-rmsv').textContent = rms+'/'+sq;

  // auto-decode progress (the click hint shows when it is off)
  const adText = {off:'Click the spectrum to tune.', paused:'auto-decode paused (trunking / scanner)', sync:'locked on '+esc(d.name),
    analog:'analog FM (AUTO still watching)', idle:'waiting for a signal · '+esc(d.name),
    probe:'trying '+esc(d.name)+' · '+Math.min(au.tried||0, au.total||0)+'/'+(au.total||0)};
  $('ad-stat').textContent = adText[au.state] || adText.off;
  renderBscan(m.bscan, d);

  // bookmark under the tuned frequency
  curFreq = inp.freq||0; curMode = d.mode||''; autoOn = au.state && au.state !== 'off';
  bmMarkCurrent();
  if (document.activeElement !== $('mode') && d.mode !== undefined) $('mode').value = d.mode;

  // slots
  const slotsEl = $('slots'); slotsEl.innerHTML='';
  const showSlot = (s, idx, stereo) => {
    if (!s) return;
    if (!stereo && idx===2) return;
    const act = s.tg || s.src;
    const el=document.createElement('div'); el.className='slot'+(d.carrier&&act?' active':'');
    const algId = s.alg && s.alg!==0x80; // ALG / key id, when the protocol sends them
    const gi = s.gi===0?'<span class="tag grp">GROUP</span>':(s.gi===1?'<span class="tag prv">PRIVATE</span>':'');
    el.innerHTML =
      '<div class="hd"><span>SLOT '+idx+(stereo?'':' / FDMA')+'</span><span>'+escH(s.burst)+' · err '+escH(s.err)+escH(s.err2)+(s.on?'':' · OFF')+'</span></div>'+
      '<div class="big">TGT <b>'+escH(s.tg)+'</b> &nbsp; SRC <b>'+escH(s.src)+'</b> '+gi+(s.enc?' <span class="tag enc">ENC '+escH(s.enc)+(algId?' · alg '+hex(s.alg)+' key '+escH(s.key):'')+'</span>':'')+'</div>'+
      (s.call&&s.call.trim()?'<div>'+escH(s.call.trim())+'</div>':'')+
      (s.alias&&s.alias.trim()?'<div class="tag">'+escH(s.alias.trim())+'</div>':'')+
      (s.gps&&s.gps.trim()?'<div class="tag">📍 '+escH(s.gps.trim())+'</div>':'');
    slotsEl.appendChild(el);
  };
  showSlot(m.slot1,1,d.stereo); showSlot(m.slot2,2,d.stereo);

  // site info
  let kv='';
  const add=(k,v)=>{ if(v!==undefined&&v!==''&&v!==0&&v!=='0') kv+='<span class="k">'+k+'</span><span class="v">'+escH(v)+'</span>'; };
  if (tr.cc_freq) add('CC freq',(tr.cc_freq/1e6).toFixed(6)+' MHz');
  if (tr.vc_freq) add('VC freq',(tr.vc_freq/1e6).toFixed(6)+' MHz');
  add('DMR CC', site.dmr_cc===16?'':site.dmr_cc);
  add('DMR', (esc(site.dmr_brand)+' '+esc(site.dmr_brand_sub)+' '+esc(site.dmr_site)).trim());
  if (site.p2_wacn) add('P25', hex(site.p2_wacn)+':'+hex(site.p2_sysid)+':'+hex(site.p2_cc)+' RFSS '+site.p2_rfss+' SITE '+site.p2_site);
  if (site.nxdn_ran>=0 && (d.sys==='NXDN')) add('NXDN RAN', site.nxdn_ran);
  if (site.nxdn_cipher) add('NXDN cipher', site.nxdn_cipher);
  $('siteinfo').innerHTML = kv;

  // active channels
  const ch=$('chanlist'); ch.innerHTML='';
  (m.channels||[]).forEach(c=>{ const s=document.createElement('span'); s.className='chip'; s.textContent=c.trim(); ch.appendChild(s); });

  // events
  renderEvents(m.events||[]);

  // reflect toggles (only if not focused/editing)
  reflect('tg-trunk', tr.enabled); reflect('tg-scan', tr.scanner);
  reflect('tg-grp', tr.group); reflect('tg-prv', tr.private); reflect('tg-data', tr.data); reflect('tg-enc', tr.enc);
  reflect('tg-allow', tr.allow_list);
  reflect('tg-autotune', fl.auto_tune); reflect('tg-autodec', fl.auto_decode); reflect('tg-autoana', fl.auto_analog);
  reflect('tg-hpfd', fl.hpfd); reflect('tg-hpf', fl.hpf); reflect('tg-lpf', fl.lpf); reflect('tg-pbf', fl.pbf);
  reflect('tg-payload', fl.payload); reflect('tg-monitor', fl.monitor); reflect('tg-inv', fl.inverted);
  reflect('tg-s1', m.slot1 && m.slot1.on); reflect('tg-s2', m.slot2 && m.slot2.on);
  reflect('tg-force', fl.forced_alg); reflect('tg-autokey', fl.auto_key);
  reflect('tg-wav', fl.rec_wav); reflect('tg-wav2', fl.rec_wav);
  const kh=m.key_hunt||{}; const el=$('autokey-stat');
  if (el){
    if (kh.locked) el.innerHTML='<span style="color:var(--good)">✓ found key '+kh.found_key+'</span>';
    else if (kh.active) el.textContent='searching… '+(kh.checked||0)+' encrypted frames checked, waiting for a pause in speech';
    else el.textContent='';
  }
  // only when the running value changes, so a pick waiting for Set isn't overwritten
  if (inp.bw && inp.bw !== shownBw){ shownBw = inp.bw; $('rtlbw').value = String(inp.bw); }
  if (inp.rate !== undefined){
    if (document.activeElement !== $('rtlrate')) $('rtlrate').value = String(inp.rate_target||0);
    $('rtlratev').textContent = 'runs at '+(inp.rate/1e6).toFixed(3);
  }
  // hardware AGC greys out the manual LNA gain; the slider then shows the last manual value
  const agc = !!inp.agc;
  reflect('tg-agc', agc); $('rtlgain').disabled = agc;
  reflect('tg-dagc', inp.dagc);
  if (document.activeElement !== $('rtlgain')){
    const g = agc ? inp.gain_manual : inp.gain;
    if (g) $('rtlgain').value = g;
    $('rtlgainv').textContent = agc ? 'auto' : gainLabel(inp.gain, inp.gain_actual);
  }
}
// manual gain, plus the step the tuner actually uses when it differs (gain_actual is in tenths of dB)
function gainLabel(g, actual){ return g+' dB'+(actual > 0 && actual !== g*10 ? ' ('+(actual/10).toFixed(1)+')' : ''); }
let shownBw = 0; // RTL bandwidth last reflected into the RF tab
function hex(n){ return (n>>>0).toString(16).toUpperCase(); }
function reflect(id, on){ const b=$(id); if(!b) return; b.classList.toggle('on', !!on); }

function renderEvents(evs){
  const el=$('events'); el.innerHTML='';
  evs.forEach(e=>{
    const div=document.createElement('div'); div.className='e c'+(e.color||4);
    let s = '['+e.slot+'] '+esc(e.text);
    if (e.msg) s+='\n    💬 '+esc(e.msg);
    if (e.alias) s+='\n    👤 '+esc(e.alias);
    if (e.gps) s+='\n    📍 '+esc(e.gps);
    if (e.note) s+='\n    ⓘ '+esc(e.note);
    div.textContent = s; el.appendChild(div);
  });
}

function logConsole(line){
  const el=$('console'); const d=document.createElement('div'); d.className='e'; d.textContent=line;
  el.appendChild(d); while (el.children.length>400) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

//---------------------------------------------------------------- bookmarks
let bms = [], bmEdit = 0, bmCurId = 0, bmMsgTimer = 0;
const MODE_LABEL = {autodecode:'Auto-detect', auto:'AUTO', tdma:'TDMA', p25p1:'P25p1', p25p2:'P25p2', dmr:'DMR', nxdn48:'NXDN48',
  nxdn96:'NXDN96', dpmr:'dPMR', ysf:'YSF', dstar:'D-STAR', m17:'M17', provoice:'ProVoice', edacs:'EDACS', analog:'Analog'};
const MODS = ['c4fm','qpsk','gfsk'];

// bookmark decoder choices: keep / auto-detect + everything the decoder dropdown offers
(function(){
  const s=$('bm-mode');
  s.add(new Option('Keep current decoder', ''));
  s.add(new Option('Auto-detect (auto-decode + analog)', 'autodecode'));
  for (const o of $('mode').options) s.add(new Option(o.text, o.value));
})();

// MHz without trailing zeros, but at least kHz resolution: 154.9875, 154.800, 446.00625
function fmtMHz(hz){ return (hz/1e6).toFixed(5).replace(/0{1,2}$/, ''); }
// nearest bookmark within 2.5 kHz (less than half the narrowest channel step)
function bmAt(f){ let best=null, bd=2501; for (const b of bms){ const d=Math.abs(b.freq-f); if (d<bd){ bd=d; best=b; } } return best; }
function bmTune(b){ send({cmd:'bm_tune', id:b.id}); $('tunefreq').value=(b.freq/1e6).toFixed(6); }
function bmMsg(t, bad){ const e=$('bm-msg'); e.textContent=t||''; e.style.color=bad?'var(--bad)':''; clearTimeout(bmMsgTimer); if (t && !bad) bmMsgTimer=setTimeout(()=>{ e.textContent=''; }, 4000); }

function bmFiltered(){
  const q=$('bm-q').value.trim().toLowerCase(), g=$('bm-grp').value;
  return bms.filter(b => (!g || b.group===g) &&
      (!q || b.name.toLowerCase().includes(q) || b.group.toLowerCase().includes(q) || (b.freq/1e6).toFixed(6).includes(q)))
    .sort((a,b) => (a.group||'￿').localeCompare(b.group||'￿') || a.freq-b.freq);
}

function renderBookmarks(){
  const groups=[...new Set(bms.map(b=>b.group).filter(x=>x))].sort();
  const sel=$('bm-grp'), keep=sel.value;
  sel.innerHTML='';
  sel.add(new Option('All groups', ''));
  for (const g of groups) sel.add(new Option(g, g));
  sel.value = groups.includes(keep) ? keep : '';
  const dl=$('bm-groups'); dl.innerHTML='';
  for (const g of groups) dl.appendChild(new Option(g));

  const list=bmFiltered(), el=$('bm-list'); el.innerHTML='';
  if (!list.length){
    const h=document.createElement('div'); h.className='hint';
    h.textContent = bms.length ? 'No bookmark matches.' : 'No bookmarks yet. Tune a channel and press ☆ or + Add.';
    el.appendChild(h);
  }
  let lastGroup = null;
  for (const b of list){
    // group headings, unless one group is picked or nothing is grouped at all
    if (!sel.value && groups.length && b.group !== lastGroup){
      const gh=document.createElement('div'); gh.className='bm-g'; gh.textContent=b.group||'Other';
      el.appendChild(gh); lastGroup=b.group;
    }
    const row=document.createElement('div'); row.className='bm'; row.dataset.id=b.id; row.title='Tune '+b.name+' ('+fmtMHz(b.freq)+' MHz)';
    const n=document.createElement('span'); n.className='bm-n'; n.textContent=b.name;
    const f=document.createElement('span'); f.className='bm-f'; f.textContent=fmtMHz(b.freq);
    row.append(n, f);
    if (b.mode){ const t=document.createElement('span'); t.className='tag'; t.textContent=MODE_LABEL[b.mode]||b.mode; row.append(t); }
    const e=document.createElement('button'); e.textContent='✎'; e.title='Edit';
    e.onclick=(ev)=>{ ev.stopPropagation(); bmFormOpen(b); };
    const x=document.createElement('button'); x.textContent='✕'; x.title='Delete';
    x.onclick=(ev)=>{ ev.stopPropagation(); if (confirm('Delete bookmark "'+b.name+'"?')) send({cmd:'bm_delete', id:b.id}); };
    row.append(e, x);
    row.onclick=()=>bmTune(b);
    el.appendChild(row);
  }
  bmCurId = -1; bmMarkCurrent();
}

// highlight the bookmark we are tuned to without rebuilding the list
function bmMarkCurrent(){
  const cur = curFreq ? bmAt(curFreq) : null, id = cur ? cur.id : 0;
  if (id === bmCurId) return;
  bmCurId = id;
  for (const r of $('bm-list').children) r.classList.toggle('on', +r.dataset.id === id);
  $('d-bm').textContent = cur ? '★ '+cur.name : '';
  $('btn-bm').textContent = cur ? '★' : '☆';
  $('btn-bm').title = cur ? 'Edit bookmark "'+cur.name+'"' : 'Bookmark this frequency';
}

function bmFormOpen(b){
  bmEdit = b.id || 0;
  $('bm-name').value = b.name || '';
  $('bm-freq').value = b.freq ? (b.freq/1e6).toFixed(6) : '';
  $('bm-mode').value = b.mode || '';
  $('bm-mod').value = b.mod || '';
  $('bm-group').value = b.group || '';
  $('bm-save').textContent = bmEdit ? 'Save changes' : 'Add bookmark';
  $('bm-form').classList.remove('hidden');
  bmMsg('');
  $('bm-name').focus();
}
function bmFormClose(){ bmEdit = 0; $('bm-form').classList.add('hidden'); }
function bmCurrent(){ return {freq:curFreq, mode: autoOn ? 'autodecode' : curMode}; }
function bmQuick(){ const cur = curFreq ? bmAt(curFreq) : null; bmFormOpen(cur || bmCurrent()); }

$('bm-add').onclick = ()=>bmFormOpen(bmCurrent());
$('btn-bm').onclick = bmQuick;
$('bm-cancel').onclick = ()=>{ bmFormClose(); bmMsg(''); };
$('bm-cur').onclick = ()=>{ const c=bmCurrent(); if (c.freq) $('bm-freq').value=(c.freq/1e6).toFixed(6); $('bm-mode').value=c.mode; };
$('bm-save').onclick = ()=>{
  const freq=parseFreq($('bm-freq').value);
  if (!freq){ bmMsg('Enter a frequency, e.g. 851.0125M.', true); return; }
  send({cmd:'bm_save', id:bmEdit, freq, name:$('bm-name').value, mode:$('bm-mode').value, mod:$('bm-mod').value, group:$('bm-group').value});
};
for (const id of ['bm-name','bm-freq','bm-group']) $(id).addEventListener('keydown', e=>{ if (e.key==='Enter') $('bm-save').click(); else if (e.key==='Escape') $('bm-cancel').click(); });
$('bm-q').oninput = renderBookmarks;
$('bm-grp').onchange = renderBookmarks;

$('bm-export').onclick = ()=>{
  const data = JSON.stringify({version:1, bookmarks: bms.map(b=>({freq:b.freq, name:b.name, group:b.group, mode:b.mode, mod:b.mod}))}, null, 2);
  const a=document.createElement('a'); a.href=URL.createObjectURL(new Blob([data], {type:'application/json'}));
  a.download='dsd-fme-bookmarks.json'; document.body.appendChild(a); a.click(); a.remove();
  setTimeout(()=>URL.revokeObjectURL(a.href), 2000);
};
$('bm-import').onclick = ()=>$('bm-file').click();
$('bm-file').onchange = async ()=>{
  const f=$('bm-file').files[0]; if (!f) return;
  const items=parseBookmarkFile(await f.text()); $('bm-file').value='';
  if (!items.length){ bmMsg('No bookmarks found in '+f.name+'.', true); return; }
  send({cmd:'bm_import', items}); bmMsg('Importing '+items.length+' bookmarks…');
};

// our export (or a bare array), SDR++ frequency_manager_config.json, or CSV lines freq,name,decoder,group
function parseBookmarkFile(text){
  const out=[];
  const push=(freq, name, group, mode, mod)=>{
    freq=Math.round(+freq); if (!(freq>0)) return;
    mode=(''+(mode||'')).toLowerCase(); mod=(''+(mod||'')).toLowerCase();
    out.push({freq, name:''+(name||''), group:''+(group||''), mode: MODE_LABEL[mode]?mode:'', mod: MODS.includes(mod)?mod:''});
  };
  let j=null; try { j=JSON.parse(text); } catch(e){}
  if (j && typeof j==='object'){
    const arr = Array.isArray(j) ? j : (Array.isArray(j.bookmarks) ? j.bookmarks : null);
    if (arr) for (const b of arr){ if (b) push(b.freq, b.name, b.group, b.mode, b.mod); }
    else if (j.lists && typeof j.lists==='object')
      for (const [list, l] of Object.entries(j.lists))
        for (const [name, b] of Object.entries((l && l.bookmarks) || {})) if (b) push(b.frequency, name, list);
    return out;
  }
  for (const line of text.split(/\r?\n/)){
    const p=line.split(',').map(s=>s.trim());
    const freq=parseFreq(p[0]); if (freq) push(freq, p[1], p[3], p[2]);
  }
  return out;
}

//---------------------------------------------------------------- band scan
const BS_PRESETS = {lmr:[[136,174],[380,512],[806,870],[896,941]], vhf:[[136,174]], uhf:[[400,470]], uhft:[[470,512]],
  '800':[[806,870]], '900':[[896,941]], ham:[[144,148],[420,450]], pmr:[[446,446.2]]};
function bsRanges(){
  if ($('bs-preset').value === 'custom'){
    const lo=parseFreq($('bs-lo').value), hi=parseFreq($('bs-hi').value);
    return lo && hi && hi > lo ? [[lo, hi]] : null;
  }
  return BS_PRESETS[$('bs-preset').value].map(([a,b]) => [Math.round(a*1e6), Math.round(b*1e6)]);
}
// exclusions: one frequency or "lo-hi" range per line (or ; separated), as [lo, hi] Hz pairs
function bsExcl(){
  const out=[];
  for (const t of $('bs-excl').value.split(/[\n;]+/)){
    const m=t.match(/^\s*(.+?)\s*(?:[-–]|\bto\b)\s*(.+?)\s*$/), a=parseFreq(m ? m[1] : t), b=m ? parseFreq(m[2]) : a;
    if (a && b) out.push([Math.min(a,b), Math.max(a,b)]);
  }
  return out;
}
function bsOpts(){ return {threshold:+$('bs-thr').value||10, skip_known:$('bs-skip').classList.contains('on'), keep_going:$('bs-keep').classList.contains('on'), exclude:bsExcl()}; }
// per-browser convenience: remember the band choice and options
function bsSave(){ try { localStorage.setItem('dsdfme.bscan', JSON.stringify({p:$('bs-preset').value, lo:$('bs-lo').value, hi:$('bs-hi').value,
  t:$('bs-thr').value, s:$('bs-skip').classList.contains('on'), k:$('bs-keep').classList.contains('on'), x:$('bs-excl').value})); } catch(e){} }
function bsCustom(){ $('bs-custom').classList.toggle('hidden', $('bs-preset').value !== 'custom'); }
(function(){ try { const s=JSON.parse(localStorage.getItem('dsdfme.bscan') || 'null'); if (!s) return;
  if (s.p in BS_PRESETS || s.p === 'custom') $('bs-preset').value=s.p;
  $('bs-lo').value=s.lo||''; $('bs-hi').value=s.hi||''; $('bs-thr').value=s.t||10; $('bs-excl').value=s.x||'';
  reflect('bs-skip', s.s); reflect('bs-keep', s.k); bsCustom(); } catch(e){} })();

$('bs-preset').onchange=()=>{ bsCustom(); bsSave(); };
for (const id of ['bs-lo','bs-hi']) $(id).onchange=bsSave;
$('bs-start').onclick=()=>{
  const r=bsRanges();
  if (!r){ $('bs-stat').textContent='Enter the range to sweep, from and to in MHz.'; $('bs-stat').style.color='var(--bad)'; return; }
  send(Object.assign({cmd:'bscan_start', ranges:r}, bsOpts())); bsSave();
};
$('bs-cont').onclick=()=>send({cmd:'bscan_continue'});
let bsCur=0, bsOff=true; // what Exclude & continue excludes: the carrier the scan is on, once stopped the tuned frequency
$('bs-excl-cont').onclick=()=>{
  const f=bsCur || (bsOff ? curFreq : 0); // while sweeping the tuned spot is the scan's own, between channels
  const el=$('bs-excl'), v=el.value.replace(/\s+$/, '');
  if (f && !bsExcl().some(([a,b]) => f >= a-3000 && f <= b+3000)) el.value=(v ? v+'\n' : '')+fmtMHz(f);
  // a running scan drops the carrier and moves on; a stopped one (tuning by hand stops it) starts again where it left off
  send(Object.assign({cmd:'bscan_opts', resume:true, ranges:bsRanges()}, bsOpts())); bsSave();
};
$('bs-stop').onclick=()=>send({cmd:'bscan_stop'});
for (const id of ['bs-skip','bs-keep']) $(id).onclick=()=>{ $(id).classList.toggle('on'); send(Object.assign({cmd:'bscan_opts'}, bsOpts())); bsSave(); };
for (const id of ['bs-thr','bs-excl']) $(id).onchange=()=>{ send(Object.assign({cmd:'bscan_opts'}, bsOpts())); bsSave(); };

function renderBscan(bs, d){
  if (!bs) return;
  const on = bs.state !== 'off', mhz = f => fmtMHz(f)+' MHz';
  $('bs-badge').textContent = {off:'off', sweep:'sweeping', probe:'probing', found:'locked', fixed:'found'}[bs.state] || bs.state;
  $('bs-badge').className = 'badge '+(bs.state==='found' || bs.state==='fixed' ? 'live' : (on ? 'warn' : ''));
  $('st-bs').textContent = bs.state==='fixed' ? 'FOUND' : 'BAND SCAN';
  $('st-bs').className = 'badge '+(bs.state==='fixed' ? 'live' : 'warn')+(on ? '' : ' hidden');
  $('bs-start').disabled = on; $('bs-stop').disabled = !on; $('bs-cont').disabled = bs.state !== 'fixed';
  bsCur = bs.state === 'fixed' ? bs.last_freq : (bs.state === 'probe' || bs.state === 'found' ? bs.freq : 0);
  bsOff = !on;
  $('bs-prog').style.width = on && bs.wins ? Math.min(100, 100*bs.win/bs.wins)+'%' : '0%';
  if (on){ // another browser may have started it: show its settings
    reflect('bs-skip', bs.skip_known); reflect('bs-keep', bs.keep_going);
    if (document.activeElement !== $('bs-thr')) $('bs-thr').value = bs.threshold;
  }
  const tail = ' · pass '+bs.pass+', window '+bs.win+'/'+bs.wins+(bs.found ? ', '+bs.found+' found' : '')+(bs.skipping ? ', '+bs.skipping+' non-digital skipped' : '')+(bs.excluded ? ', '+bs.excluded+' excluded' : '');
  let t, short = '';
  if (bs.state === 'sweep'){ t = 'Sweeping '+mhz(bs.freq)+tail; short = 'Band scan: sweeping '+mhz(bs.freq); }
  else if (bs.state === 'probe'){
    t = 'Signal at '+mhz(bs.freq)+': trying '+esc(d.name)+' ('+bs.cand+'/'+bs.ncand+')'+(bs.left ? ', '+bs.left+' more in this window' : '')+tail;
    short = 'Band scan: trying '+esc(d.name)+' on '+mhz(bs.freq);
  }
  else if (bs.state === 'found'){ t = 'Decoding '+(bs.desc || esc(d.name))+' on '+mhz(bs.freq)+', collecting ids for the bookmark…'; short = 'Band scan: locked on '+mhz(bs.freq); }
  else if (bs.state === 'fixed'){ t = 'Found '+bs.last_name+' on '+mhz(bs.last_freq)+' and bookmarked it (group "Band scan"). Continue keeps looking.'; short = 'Band scan: found '+bs.last_name; }
  else if (bs.msg) t = 'Band scan '+bs.msg;
  else t = 'Sweeps the bands with the RTL-SDR, tries every signal it sees with the digital decoders, and stops on the first one that decodes, saving it as a bookmark.'
    + (bs.found ? ' Last find: '+bs.last_name+' on '+mhz(bs.last_freq)+'.' : '');
  $('bs-stat').textContent = t;
  $('bs-stat').style.color = !on && bs.msg ? 'var(--bad)' : '';
  if (short) $('ad-stat').textContent = short;
}

//---------------------------------------------------------------- files
function renderFiles(m){
  const el=$('filelist'); el.innerHTML='';
  if (!m.files || !m.files.length){ el.innerHTML='<div class="hint">No files.</div>'; return; }
  m.files.sort((a,b)=>b.mtime-a.mtime);
  m.files.forEach(f=>{
    const a=document.createElement('a');
    const q = 'dl=1'+(token?('&t='+token):'');
    a.href='/dl/'+m.area+'/'+encodeURIComponent(f.name)+'?'+q;
    a.textContent = f.name+'  ('+(f.size/1024).toFixed(0)+' KB)';
    el.appendChild(a);
  });
}

//---------------------------------------------------------------- controls
function bindToggle(id, cmd, key){ $(id).onclick=()=>{ const on=!$(id).classList.contains('on'); const o={cmd}; o[key]=on; send(o); $(id).classList.toggle('on',on); }; }

$('btn-tune').onclick=()=>{ const f=parseFreq($('tunefreq').value); if(f) tuneTo(f); };
$('tunefreq').addEventListener('keydown',e=>{ if(e.key==='Enter'){ const f=parseFreq($('tunefreq').value); if(f) tuneTo(f);} });
// 851.0125M, 154875k, 154.875 MHz, 851012500 (Hz) or 154.875 (MHz); commas are thousands
// separators (851,012,500) unless there is a single one and no dot (154,875 = 154.875)
function parseFreq(s){ s=(s||'').trim().toLowerCase().replace(/\s+/g,'').replace(/hz$/,''); if(!s) return null; let mult=1;
  const commas=(s.match(/,/g)||[]).length; s = commas===1 && !s.includes('.') ? s.replace(',','.') : s.replace(/,/g,'');
  const last=s.slice(-1); if(last==='m'){mult=1e6;s=s.slice(0,-1);} else if(last==='k'){mult=1e3;s=s.slice(0,-1);} else if(last==='g'){mult=1e9;s=s.slice(0,-1);}
  let v=parseFloat(s); if(isNaN(v)) return null; v*=mult; if(mult===1 && v<1e6) v*=1e6; return Math.round(v); }

$('mode').onchange=()=>send({cmd:'mode', mode:$('mode').value});
$('modu').onchange=()=>send({cmd:'modulation', mod:$('modu').value});
$('tg-inv').onclick=()=>{ const on=!$('tg-inv').classList.contains('on'); send({cmd:'inversion', inverted:on}); $('tg-inv').classList.toggle('on',on); };
$('tg-s1').onclick=()=>{ const on=!$('tg-s1').classList.contains('on'); send({cmd:'slots', slot1:on}); $('tg-s1').classList.toggle('on',on); };
$('tg-s2').onclick=()=>{ const on=!$('tg-s2').classList.contains('on'); send({cmd:'slots', slot2:on}); $('tg-s2').classList.toggle('on',on); };
$('gdig').oninput=()=>{ const v=+$('gdig').value; $('gdigv').textContent=v?v:'auto'; send({cmd:'gain', digital:v}); };
$('gana').oninput=()=>{ const v=+$('gana').value; $('ganav').textContent=v; send({cmd:'gain', analog:v}); };
bindToggle('tg-hpfd','filters','hpfd'); bindToggle('tg-hpf','filters','hpf');
bindToggle('tg-lpf','filters','lpf'); bindToggle('tg-pbf','filters','pbf');
bindToggle('tg-payload','payload','enable'); bindToggle('tg-monitor','monitor','enable');
function toggleWav(id){ const on=!$(id).classList.contains('on'); send({cmd:'per_call_wav', enable:on}); reflect('tg-wav',on); reflect('tg-wav2',on); }
$('tg-wav').onclick=()=>toggleWav('tg-wav'); $('tg-wav2').onclick=()=>toggleWav('tg-wav2');
$('btn-resetev').onclick=()=>send({cmd:'reset_events'});
$('btn-p2').onclick=()=>send({cmd:'p2_params', wacn:parseInt($('p2wacn').value,16)||0, sysid:parseInt($('p2sys').value,16)||0, cc:parseInt($('p2cc').value,16)||0});

// RF
$('rtlgain').oninput=()=>{ $('rtlgainv').textContent=$('rtlgain').value+' dB'; };
$('rtlgain').onchange=()=>{ if (!$('tg-agc').classList.contains('on')) send({cmd:'rtl', gain:+$('rtlgain').value}); };
// gain 0 is the hardware AGC; unticking it goes back to the slider's manual gain
$('tg-agc').onclick=()=>{ const on=!$('tg-agc').classList.contains('on'); reflect('tg-agc', on); $('rtlgain').disabled=on;
  $('rtlgainv').textContent = on ? 'auto' : $('rtlgain').value+' dB'; send({cmd:'rtl', gain: on ? 0 : +$('rtlgain').value}); };
// RTL2832 digital AGC, independent of the tuner gain (the LNA slider stays as it is)
$('tg-dagc').onclick=()=>{ const on=!$('tg-dagc').classList.contains('on'); reflect('tg-dagc', on); send({cmd:'rtl', dagc:on}); };
$('btn-ppm').onclick=()=>send({cmd:'rtl', ppm:+$('rtlppm').value});
$('rtlvol').onchange=()=>send({cmd:'rtl', vol:+$('rtlvol').value});
$('rtlrate').onchange=()=>{ send({cmd:'rtl', rate:+$('rtlrate').value}); $('rtlrate').blur(); };
$('btn-bw').onclick=()=>send({cmd:'rtl', bw:+$('rtlbw').value});
$('btn-sq').onclick=()=>send({cmd:'rtl', squelch:+$('rtlsq').value});
$('btn-smb').onclick=()=>send({cmd:'setmod_bw', hz:+$('setmodbw').value});

// Trunk / scan
$('tg-trunk').onclick=()=>{ const on=!$('tg-trunk').classList.contains('on'); send({cmd:'trunk', enable:on}); };
$('tg-scan').onclick=()=>{ const on=!$('tg-scan').classList.contains('on'); send({cmd:'scanner', enable:on}); };
$('btn-cc').onclick=()=>send({cmd:'return_cc'});
$('btn-cycle').onclick=()=>send({cmd:'cycle'});
for (const [id,key] of [['tg-grp','group'],['tg-prv','private'],['tg-data','data'],['tg-enc','enc'],['tg-allow','allow_list']])
  $(id).onclick=(function(id,key){ return ()=>{ const on=!$(id).classList.contains('on'); const o={cmd:'trunk_opts'}; o[key]=on; send(o); $(id).classList.toggle('on',on); }; })(id,key);
$('btn-hang').onclick=()=>send({cmd:'hangtime', seconds:+$('hang').value});
$('btn-tgh').onclick=()=>send({cmd:'tg_hold', tg:+$('tghold').value});
$('btn-chan').onclick=()=>send({cmd:'csv', kind:'channel', text:$('csv-chan').value});
$('btn-group').onclick=()=>send({cmd:'csv', kind:'group', text:$('csv-group').value});
$('btn-scanset').onclick=()=>{ const fs=$('scanlist').value.split(/\s+/).map(parseFreq).filter(x=>x); send({cmd:'scan_set', freqs:fs}); };
$('btn-scanbm').onclick=()=>{ $('scanlist').value=bmFiltered().slice(0,26).map(b=>(b.freq/1e6).toFixed(6)+'M').join('\n'); };

// auto
$('tg-autotune').onclick=()=>{ const on=!$('tg-autotune').classList.contains('on'); send({cmd:'auto_tune', enable:on}); $('tg-autotune').classList.toggle('on',on); };
$('tg-autodec').onclick=()=>{ const on=!$('tg-autodec').classList.contains('on'); send({cmd:'auto_decode', enable:on}); $('tg-autodec').classList.toggle('on',on); };
$('tg-autoana').onclick=()=>{ const on=!$('tg-autoana').classList.contains('on'); send({cmd:'auto_analog', enable:on}); $('tg-autoana').classList.toggle('on',on); };

// keys
const khints={bp:'Decimal key id (0-255)',scrambler:'Decimal (1-32767)',rc4:'Hex, e.g. 0102030405',des:'Hex 16 chars',aes:'Hex, space every 16 (128/256-bit)',hytera:'Hex 10/32/64 chars',tyt_bp:'Hex 4 chars',tyt_ap:'Hex 32/64 chars',tyt_ep:'Hex 32 chars',retevis:'Hex 32/64 chars',baofeng:'Hex 32/64 chars',anytone:'Hex 4 chars',kenwood:'Decimal (15-bit)',csi:'Hex 18 chars (72-bit)',vertex:'10:xxxxxxxxxx or 64:...',auctus:'8 chars',straight:'len:hexbytes[:discard]',ras:'password (6-24 chars)'};
$('ktype').onchange=()=>{ $('khint').textContent=khints[$('ktype').value]||''; };
$('ktype').onchange();
$('btn-key').onclick=()=>send({cmd:'key', keytype:$('ktype').value, value:$('kval').value});
$('btn-keyclear').onclick=()=>send({cmd:'key', keytype:'clear'});
$('tg-autokey').onclick=()=>{ const on=!$('tg-autokey').classList.contains('on'); send({cmd:'auto_key', enable:on}); $('tg-autokey').classList.toggle('on',on); };
$('tg-force').onclick=()=>{ const on=!$('tg-force').classList.contains('on'); send({cmd:'key', keytype: on?'force':'unforce'}); $('tg-force').classList.toggle('on',on); };
$('btn-keycsv').onclick=()=>send({cmd:'csv', kind:$('kcsvkind').value, text:$('csv-keys').value});

// files
$('btn-refresh').onclick=()=>send({cmd:'list_files', area:'wav'});

// spectrum config
function pushSpec(){ send({cmd:'spectrum', fft:+$('fftsize').value, fps:+$('fps').value, avg:+$('avg').value}); }
$('fftsize').onchange=pushSpec; $('fps').onchange=pushSpec; $('avg').onchange=pushSpec;

// tabs
$('tabs').addEventListener('click', e=>{ if(e.target.tagName!=='BUTTON') return;
  const t=e.target.dataset.t;
  for (const b of $('tabs').children) b.classList.toggle('on', b.dataset.t===t);
  for (const p of document.querySelectorAll('.tabpane')) p.classList.toggle('on', p.dataset.t===t);
  if (t==='files') send({cmd:'list_files', area:'wav'});
});

// login
$('pw-go').onclick=()=>send({cmd:'auth', password:$('pw').value});
$('pw').addEventListener('keydown',e=>{ if(e.key==='Enter') $('pw-go').click(); });

sizeCanvas();
renderBookmarks();
connect();
