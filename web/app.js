/* DSD-FME web client */
'use strict';
const $ = (id) => document.getElementById(id);
// a symbol from the sprite at the top of index.html
const icon = (n) => '<svg class="i" aria-hidden="true"><use href="#i-'+n+'"/></svg>';
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
  ws.onopen = () => { setConn('Connecting', 'warn'); resync(); };
  ws.onclose = () => { setConn('Offline','off'); authed=false; setTimeout(connect, 1500); };
  ws.onerror = () => {};
  ws.onmessage = onMessage;
}
function send(obj){ if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function subscribe(){ send({cmd:'subscribe', fft:true, audio:audioOn, console:true, events:actKind==='log'}); }
function resync(){ subscribe(); send({cmd:'get_status'}); send({cmd:'bm_list'}); send({cmd:'activity'}); }

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
    if (needPassword && !authed){ loginShow(true); }
    else { authed = true; loginShow(false); setConn('Connected','live'); resync(); }
    if (m.rtl === false) $('tunefreq').placeholder = 'RIGCTL tune (SDR++ etc)';
  }
  else if (m.type === 'auth'){
    if (m.ok){ authed = true; loginShow(false); $('pw-err').textContent=''; setConn('Connected','live'); resync(); }
    else { $('pw-err').textContent = 'Wrong password. Check it and connect again.'; $('pw').select(); }
  }
  else if (m.type === 'status'){ authed = true; setConn('Connected','live'); renderStatus(m); }
  else if (m.type === 'activity'){ handleActivity(m); }
  else if (m.type === 'system'){ renderSystem(m); }
  else if (m.type === 'groups'){ groupsSet(m); }
  else if (m.type === 'files'){ renderFiles(m); }
  else if (m.type === 'bookmarks'){ bms = m.items || []; $('bm-path').textContent = m.path || '—'; renderBookmarks(); }
  else if (m.type === 'bm_saved'){ bmFormClose(); bmMsg(m.warn ? 'Saved, but '+m.warn+'.' : 'Saved.', !!m.warn); }
  else if (m.type === 'bm_error'){ bmMsg('Not saved: '+m.msg+'.', true); }
  else if (m.type === 'bm_imported'){ bmMsg(m.msg); }
}

function setConn(txt, cls){ const b=$('st-conn'); b.textContent=txt; b.className='badge '+cls; }
// the password prompt (and the map) keep keyboard focus out of the page behind them
function loginShow(on){ $('login').classList.toggle('hidden', !on); pageInert(on || MAP.open); if (on) $('pw').focus(); }
function pageInert(on){ $('top').inert = on; $('main').inert = on; }

//---------------------------------------------------------------- spectrum
const spec = $('spec'), wf = $('wf');
const sctx = spec.getContext('2d'), wctx = wf.getContext('2d');
let wfImg = null, dpr = 1;

// the trace is drawn at the screen's pixel density; the waterfall keeps one row per CSS pixel,
// and its history survives a resize (rows keep their height, the span is stretched to the new width)
function sizeCanvas(){
  dpr = Math.min(window.devicePixelRatio || 1, 3);
  const sw = Math.round(spec.clientWidth*dpr), sh = Math.round(spec.clientHeight*dpr);
  if (sw && sh && (spec.width !== sw || spec.height !== sh)){ spec.width = sw; spec.height = sh; }
  // no layout size (hidden for a moment): keep the canvas and its history as they are
  const w = wf.clientWidth|0, h = wf.clientHeight|0;
  if (w && h && (wf.width !== w || wf.height !== h)){
    let old = null;
    if (wf.width && wf.height){ old = document.createElement('canvas'); old.width = wf.width; old.height = wf.height; old.getContext('2d').drawImage(wf, 0, 0); }
    wf.width = w; wf.height = h;
    if (old && w && h) wctx.drawImage(old, 0, 0, old.width, old.height, 0, 0, w, old.height);
  }
  wfImg = wf.width > 0 ? wctx.createImageData(wf.width, 1) : null; // zero while the page has no layout width
}
window.addEventListener('resize', sizeCanvas);

// waterfall palette as a 256-entry table: display glass, a dark indigo where the noise sits
// (about 0.3 with the default range), then plum, ember, amber and white hot for signals
const WF_LUT = (function(){
  const stops = [[0,[8,8,10]],[0.22,[12,12,18]],[0.34,[30,26,52]],[0.46,[92,34,98]],[0.6,[196,62,62]],[0.76,[252,152,44]],[1,[255,240,190]]];
  const t = new Uint8ClampedArray(256*3);
  for (let k=0, j=1; k<256; k++){
    const n = k/255; while (j < stops.length-1 && n > stops[j][0]) j++;
    const [p0, a] = stops[j-1], [p1, b] = stops[j], f = (n-p0)/(p1-p0);
    for (let c=0; c<3; c++) t[k*3+c] = a[c]+(b[c]-a[c])*f;
  }
  return t;
})();
function dbColor(n){ // n in 0..1
  const k = Math.max(0, Math.min(255, Math.round(n*255)))*3;
  return [WF_LUT[k], WF_LUT[k+1], WF_LUT[k+2]];
}

// FFT bins down to n pixels, keeping the strongest bin under each pixel so narrow carriers don't drop out
function binsTo(vals, bins, n, out){
  for (let x=0; x<n; x++){
    let i = (x*bins/n)|0; const e = Math.min(bins, Math.max(i+1, ((x+1)*bins/n)|0));
    let v = vals[i]; for (i++; i<e; i++) if (vals[i] > v) v = vals[i];
    out[x] = v;
  }
  return out;
}
let rowS = new Float32Array(0), rowW = new Float32Array(0);

function handleFFT(dv, buf){
  const bins = dv.getUint16(2, true);
  const center = dv.getUint32(4, true);
  const rate = dv.getUint32(8, true);
  const dbmin = dv.getFloat32(12, true);
  const u8 = new Uint8Array(buf, 16, bins);
  const vals = new Float32Array(bins);
  let mn = 999;
  for (let i=0;i<bins;i++){ const d = dbmin + u8[i]*0.5; vals[i]=d; if (d<mn) mn=d; }
  noiseFloor = lastFFT ? noiseFloor*0.9 + mn*0.1 : mn; // the first frame sets it, so the waterfall doesn't start too bright
  lastFFT = {bins, center, rate, dbmin, vals};
  drawSpectrum();
  setHTML($('specmeta'), '<b>'+(center/1e6).toFixed(4)+' MHz</b><span>'+(rate/1e6).toFixed(2)+' MS/s</span><span>'+bins+' bins</span>');
}

function drawSpectrum(){
  if (!lastFFT || !spec.width || !wfImg || wfImg.width !== wf.width) return;
  const {bins, center, rate, vals} = lastFFT;
  const N = spec.width, W = N/dpr, H = spec.height/dpr; // N device pixels, W x H CSS pixels
  const range = +$('dbrange').value;
  const floor = noiseFloor - 3, top = floor + range;
  sctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  sctx.clearRect(0,0,W,H);
  // grid
  sctx.strokeStyle = 'rgba(255,255,255,0.045)'; sctx.lineWidth=1;
  for (let g=1;g<6;g++){ const x=Math.round(W*g/6)+0.5; sctx.beginPath(); sctx.moveTo(x,0); sctx.lineTo(x,H); sctx.stroke(); }
  // tuned frequency
  sctx.strokeStyle = 'rgba(255,176,46,0.4)'; sctx.beginPath(); sctx.moveTo(Math.round(W/2)+0.5,0); sctx.lineTo(Math.round(W/2)+0.5,H); sctx.stroke();
  // trace, one point per device pixel
  if (rowS.length !== N) rowS = new Float32Array(N);
  binsTo(vals, bins, N, rowS);
  sctx.beginPath();
  for (let k=0;k<N;k++){
    const n = (rowS[k]-floor)/(top-floor);
    const y = H - Math.max(0,Math.min(1,n))*H;
    if (k===0) sctx.moveTo(0,y); else sctx.lineTo(k/dpr,y);
  }
  sctx.lineTo(W,H); sctx.lineTo(0,H); sctx.closePath();
  sctx.fillStyle = 'rgba(255,176,46,0.06)'; sctx.fill();
  sctx.beginPath();
  for (let k=0;k<N;k++){
    const n = (rowS[k]-floor)/(top-floor);
    const y = H - Math.max(0,Math.min(1,n))*H;
    if (k===0) sctx.moveTo(0,y); else sctx.lineTo(k/dpr,y);
  }
  sctx.strokeStyle = '#ffb02e'; sctx.lineWidth=1.25; sctx.stroke();
  drawMarks(center, rate, W, H);
  // waterfall row
  const Wf = wf.width;
  const img = wfImg;
  if (rowW.length !== Wf) rowW = new Float32Array(Wf);
  binsTo(vals, bins, Wf, rowW);
  for (let x=0;x<Wf;x++){
    const c = dbColor((rowW[x]-floor)/(top-floor));
    const o = x*4; img.data[o]=c[0]; img.data[o+1]=c[1]; img.data[o+2]=c[2]; img.data[o+3]=255;
  }
  wctx.drawImage(wf, 0, 0, Wf, wf.height-1, 0, 1, Wf, wf.height-1);
  wctx.putImageData(img, 0, 0);
}

// bookmark markers inside the visible span, and while trunking the control channel and the channels
// of calls on air (green, brightest for the one followed); labels step down a row when they would
// overlap (CSS pixels). Only bookmark marks tune when clicked
let marks = [];
const MARK_STYLE = {bm:['rgba(108,184,255,0.55)', '#8cc8ff', [3,3]], cc:['rgba(223,226,220,0.45)', '#dfe2dc', [2,4]],
  air:['rgba(79,224,141,0.5)', '#7fe3a8', []], fol:['rgba(79,224,141,0.95)', '#4fe08d', []]};
function drawMarks(center, rate, W, H){
  marks = [];
  if (!rate) return;
  const lo = center - rate/2, inSpan = f => f >= lo && f <= lo + rate;
  const vis = bms.filter(b => inSpan(b.freq)).map(b => ({freq:b.freq, name:b.name, k:'bm', b}))
    .concat(trkMarks.filter(t => inSpan(t.freq))).sort((a,b) => a.freq - b.freq);
  if (!vis.length) return;
  sctx.font = '500 11.5px "Barlow Semi Condensed",system-ui,sans-serif'; sctx.textBaseline = 'middle';
  const rowEnd = [-1e9, -1e9, -1e9, -1e9];
  for (const m of vis){
    const [line, text, dash] = MARK_STYLE[m.k];
    const x = Math.round((m.freq - lo) / rate * W) + 0.5;
    sctx.strokeStyle = line; sctx.setLineDash(dash); sctx.lineWidth = m.k==='fol' ? 2 : 1;
    sctx.beginPath(); sctx.moveTo(x, 0); sctx.lineTo(x, H); sctx.stroke(); sctx.setLineDash([]); sctx.lineWidth = 1;
    const tw = Math.min(150, sctx.measureText(m.name).width + 8);
    const lx = Math.max(0, Math.min(W - tw, x + 2));
    let row = rowEnd.findIndex(e => lx > e + 2);
    if (row < 0) row = rowEnd.indexOf(Math.min(...rowEnd)); // all rows taken: the one that frees up first
    rowEnd[row] = lx + tw;
    const ly = 4 + row*15;
    sctx.fillStyle = 'rgba(15,16,15,0.84)'; sctx.fillRect(lx, ly, tw, 14);
    sctx.fillStyle = text; sctx.fillText(m.name, lx + 4, ly + 7.5, tw - 8);
    marks.push({x, lx, ly, tw, b:m.b, m});
  }
}
function markAt(ev){
  const r = spec.getBoundingClientRect(), x = ev.clientX - r.left, y = ev.clientY - r.top;
  for (const m of marks){
    if (ev.target === spec && x >= m.lx && x <= m.lx + m.tw && y >= m.ly && y <= m.ly + 14) return m;
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
  el.addEventListener('click', (ev)=>{ const m=markAt(ev); if (m){ if (m.b) bmTune(m.b); return; } const f=specToFreq(ev); if (f) tuneTo(f); });
  el.addEventListener('mousemove', (ev)=>{
    const f=specToFreq(ev); const cur=$('cursor'); if(!f){cur.style.display='none';return;}
    const r=$('specbox').getBoundingClientRect(); cur.style.display='block';
    cur.style.left=(ev.clientX-r.left)+'px';
    const m=markAt(ev);
    $('specinfo').textContent = m ? m.m.name+'  '+fmtMHz(m.m.freq)+' MHz' : (f/1e6).toFixed(5)+' MHz';
  });
  el.addEventListener('mouseleave', ()=>{ $('cursor').style.display='none'; $('specinfo').textContent=''; });
}
function tuneTo(freq){ send({cmd:'tune', freq}); $('tunefreq').value=(freq/1e6).toFixed(6); }

//---------------------------------------------------------------- audio
let actx=null, gain=null, playTime=0, audioOn=false;
function audioStart(){
  if (!actx){ actx=new (window.AudioContext||window.webkitAudioContext)(); gain=actx.createGain(); gain.connect(actx.destination); gain.gain.value=+$('vol').value/100; }
  actx.resume(); audioOn=true; playTime=0;
  $('btn-audio').innerHTML=icon('spk')+'Audio on'; reflect('btn-audio', true);
  subscribe();
}
function audioStop(){ audioOn=false; $('btn-audio').innerHTML=icon('spk-off')+'Audio off'; reflect('btn-audio', false); subscribe(); }
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
  setTxt($('st-carrier'), d.carrier ? 'Sync' : (ana ? 'Analog' : 'No sync'));
  $('st-carrier').className = 'badge '+(d.carrier?'live':(ana?'warn':''));
  setTxt($('st-mode'), esc(d.name) || '—');
  // the readout's unlit segments are "8888.888888"; no frequency (non-RTL input) leaves them all dark
  setTxt($('d-freq'), inp.freq > 0 ? (inp.freq/1e6).toFixed(6) : '');
  $('d-sys').textContent = ana ? 'Analog FM' : (esc(d.sys) || '—');
  $('d-ftype').textContent = esc(d.ftype).trim()||'—';
  $('d-mod').textContent = esc(d.mod)+', '+esc(d.rate)+' sps';
  $('d-level').style.width = (d.level||0)+'%';
  const sq=inp.squelch||1, rms=inp.rms||0;
  $('d-rms').style.width = Math.min(100, rms/ (sq*2||200) *100)+'%';
  $('d-rmsv').textContent = rms+' / '+sq;

  // auto-decode progress (the click hint shows when it is off)
  const adText = {off:'Click the spectrum to tune.', paused:'Auto-decode paused while trunking or scanning', sync:'Locked on '+esc(d.name),
    analog:'Analog FM, AUTO still watching', idle:'Waiting for a signal ('+esc(d.name)+')',
    probe:'Trying '+esc(d.name)+', '+Math.min(au.tried||0, au.total||0)+' of '+(au.total||0)};
  $('ad-stat').textContent = adText[au.state] || adText.off;
  renderBscan(m.bscan, d);

  // bookmark under the tuned frequency
  curFreq = inp.freq||0; curMode = d.mode||''; autoOn = au.state && au.state !== 'off';
  bmMarkCurrent();
  if (document.activeElement !== $('mode') && d.mode !== undefined) $('mode').value = d.mode;

  // slots: two fixed boxes, updated in place
  slotSet(1, m.slot1, d, true); slotSet(2, m.slot2, d, d.stereo);

  // site info
  let kv='';
  const add=(k,v)=>{ if(v!==undefined&&v!==''&&v!==0&&v!=='0') kv+='<span class="k">'+k+'</span><span class="v">'+escH(v)+'</span>'; };
  // the system identity is in the system picture below (#sysinfo), trunking in its own card
  if (site.nxdn_cipher && d.sys==='NXDN') add('NXDN cipher', site.nxdn_cipher);
  $('siteinfo').innerHTML = kv;

  // active channels
  const ch=$('chanlist'); ch.innerHTML='';
  (m.channels||[]).forEach(c=>{ const s=document.createElement('span'); s.className='chip'; s.textContent=c.trim(); ch.appendChild(s); });

  // the decoder's own event history, while that view is open
  if (actKind === 'log') renderEvents(m.events||[]);

  // reflect toggles (only if not focused/editing)
  renderTrunk(m);
  reflect('tg-scan', tr.scanner);
  reflect('tg-autotune', fl.auto_tune); reflect('tg-autodec', fl.auto_decode); reflect('tg-autoana', fl.auto_analog);
  reflect('tg-hpfd', fl.hpfd); reflect('tg-hpf', fl.hpf); reflect('tg-lpf', fl.lpf); reflect('tg-pbf', fl.pbf);
  reflect('tg-payload', fl.payload); reflect('tg-monitor', fl.monitor); reflect('tg-inv', fl.inverted);
  reflect('tg-s1', m.slot1 && m.slot1.on); reflect('tg-s2', m.slot2 && m.slot2.on);
  reflect('tg-force', fl.forced_alg); reflect('tg-autokey', fl.auto_key);
  reflect('tg-wav', fl.rec_wav); reflect('tg-wav2', fl.rec_wav);
  const kh=m.key_hunt||{}; const el=$('autokey-stat');
  if (el){
    if (kh.locked) setHTML(el, '<span class="found">'+icon('check')+' Found key '+kh.found_key+'</span>');
    else if (kh.active) setHTML(el, 'Searching: '+(kh.checked||0)+' encrypted frames checked, waiting for a pause in speech');
    else setHTML(el, '');
  }
  // only when the running value changes, so a pick waiting for Set isn't overwritten
  if (inp.bw && inp.bw !== shownBw){ shownBw = inp.bw; $('rtlbw').value = String(inp.bw); }
  if (inp.rate !== undefined){
    if (document.activeElement !== $('rtlrate')) $('rtlrate').value = String(inp.rate_target||0);
    $('rtlratev').textContent = 'Running at '+(inp.rate/1e6).toFixed(3)+' MSPS';
  }
  // squelch: Auto floor and Channel floor need the RTL spectrum; the box shows the running level unless it is being edited
  const rtl = inp.type === 'rtl';
  reflect('tg-sqauto', inp.sql_auto); $('tg-sqauto').disabled = !rtl; $('btn-sqchan').disabled = !rtl;
  if (document.activeElement !== $('rtlsq')) $('rtlsq').value = inp.squelch;
  $('rtlsq').step = [1,2,5,10,20,50,100,200,500].filter(s => s <= inp.squelch/40).pop() || 1; // an arrow press moves it ~0.2-0.5 dB
  if (inp.sql_margin != null && document.activeElement !== $('sqmargin')) $('sqmargin').value = String(inp.sql_margin);
  setTxt($('sqfloor'), !rtl ? 'Needs the RTL-SDR input.' : 'Noise floor '+(inp.sql_floor ? inp.sql_floor+' RMS, '+(+inp.sql_floor_db).toFixed(1)+' dB' : '—'));
  setTxt($('sqchan'), !rtl ? '' : 'This frequency '+(inp.sql_chan ? inp.sql_chan+' RMS' : '—'));
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

//---------------------------------------------------------------- slot boxes
// built once; every line is always there, empty or not, so a call starting or ending moves nothing
// target and source are 8-digit segment fields: unlit segments when there is no call
function slotInit(){
  for (const i of [1,2]) $('slot'+i).innerHTML=
    '<div class="hd"><span class="nm">Slot '+i+'</span><span class="tm"></span><span class="bu"></span></div>'+
    '<div class="big"><span class="fld"><span class="lb">Target</span><b class="tg seg" data-ghost="88888888"></b></span>'+
    '<span class="fld"><span class="lb">Source</span><b class="sr seg" data-ghost="88888888"></b></span></div>'+
    '<div class="ln tags"></div><div class="ln l1"></div><div class="ln l2"></div><div class="ln l3"></div>';
}
function setTxt(el, t, title){ if (el.textContent !== t) el.textContent = t; if (title !== undefined && el.title !== title) el.title = title; }
function setHTML(el, h){ if (el._h !== h){ el._h = h; el.innerHTML = h; } }
function slotSet(i, s, d, used){
  const el=$('slot'+i), q=c=>el.querySelector('.'+c);
  used = used && !!s;
  el.classList.toggle('off', !used);
  if (!used){
    el.classList.remove('active');
    setTxt(q('nm'), 'Slot '+i); setTxt(q('tm'), ''); setTxt(q('bu'), ''); setTxt(q('tg'), ''); setTxt(q('sr'), '');
    setHTML(q('tags'), s ? '<span class="hint">No second timeslot on this channel</span>' : '');
    for (const c of ['l1','l3']) setTxt(q(c), '', '');
    setHTML(q('l2'), ''); // through setHTML, or the same position coming back later would not be drawn
    return;
  }
  const act = !!(s.tg || s.src), lc = d.carrier && act ? liveCalls.get(d.stereo ? i : 0) : null;
  el.classList.toggle('active', !!(d.carrier && act));
  setTxt(q('nm'), 'Slot '+i+(d.stereo ? '' : ' (FDMA)'));
  setTxt(q('tm'), lc ? fmtClock(Date.now()/1000-lc.t) : '');
  setTxt(q('bu'), (s.burst||'')+' err '+esc(s.err)+esc(s.err2)+(s.on ? '' : ' off'));
  setTxt(q('tg'), s.tg ? String(s.tg) : ''); setTxt(q('sr'), s.src ? String(s.src) : '');
  const algId = s.alg && s.alg!==0x80; // ALG / key id, when the protocol sends them
  //the decoder keeps the call type after the call; only show it while the slot has ids
  setHTML(q('tags'), !act ? '' : (s.gi===0?'<span class="tag grp">Group</span>':(s.gi===1?'<span class="tag prv">Private</span>':''))+
    (s.enc?'<span class="tag enc">'+icon('lock')+escH(s.enc)+(algId?', alg '+hex(s.alg)+' key '+escH(s.key):'')+'</span>':''));
  const l1=[(s.alias||'').trim(), (s.call||'').trim()].filter(x=>x).join(' · ');
  setTxt(q('l1'), l1, l1);
  const gps=(s.gps||'').trim();
  setHTML(q('l2'), !gps ? '' : (lc && lc.lat!=null ? '<button class="gps" data-lat="'+lc.lat+'" data-lon="'+lc.lon+'" title="Show on the map">'+icon('pin')+'<span class="ell">'+escH(gps)+'</span></button>' : icon('pin')+' '+escH(gps)));
  const data=(s.data||'').trim();
  setTxt(q('l3'), data ? 'Last data: '+data : '', data);
}
$('slots').addEventListener('click', e=>{ const a=e.target.closest('[data-lat]'); if (a) mapOpen({lat:+a.dataset.lat, lon:+a.dataset.lon}); });
// toggle keys light their LED with the "on" class; aria-pressed tells assistive tech the same
function reflect(id, on){
  const b=$(id); if(!b) return; b.classList.toggle('on', !!on);
  const v=String(!!on); if (b.tagName==='BUTTON' && b.getAttribute('aria-pressed')!==v) b.setAttribute('aria-pressed', v);
}

function renderEvents(evs){
  const el=$('events'); el.innerHTML='';
  if (!evs.length) el.innerHTML='<div class="acthint">No decoder events yet.</div>';
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

//---------------------------------------------------------------- activity
// calls, decoded data and signalling from the decoder; an item that comes again (same id) replaces
// the earlier version, which is how calls update until they end and packets show their retries
const acts = new Map(), actEls = new Map(), actOpen = new Set(), liveCalls = new Map();
const onAir = new Map(); // radio -> its live call (voice heard here, or a trunked grant)
let actKind = '', actErr = false;
const ACT_KEEP = 800, ACT_SHOW = 500;

function handleActivity(m){
  if (m.reset){ acts.clear(); radios.clear(); liveCalls.clear(); onAir.clear(); }
  for (const it of m.items){
    const fresh = !acts.has(it.id), old = acts.get(it.id);
    // voice calls heard while trunking belong in the talkgroup list (they are on the system's channels)
    if (it.k==='call' && it.p!=='P25' && (trk.enabled || (old && old.trkd))) it.trkd = 1;
    acts.set(it.id, it); radioSeen(it, fresh);
    if (it.k !== 'call') continue;
    if (it.p !== 'P25'){ // voice on this channel (trunked grants seen on a P25 control channel are "P25")
      const slot = it.s || 0;
      if (it.live) liveCalls.set(slot, it); else if (liveCalls.get(slot) && liveCalls.get(slot).id === it.id) liveCalls.delete(slot);
    }
    const who = it.srcs || (it.src>0 ? String(it.src) : '');
    if (who){ if (it.live) onAir.set(who, it); else if (onAir.get(who) && onAir.get(who).id === it.id) onAir.delete(who); }
  }
  if (acts.size > ACT_KEEP){ const ids=[...acts.keys()].sort((a,b)=>a-b); for (const id of ids.slice(0, acts.size-ACT_KEEP)) acts.delete(id); }
  if (m.reset) renderActs(); else for (const it of m.items) actPut(it);
  $('act-count').textContent = acts.size;
  renderRadios();
  renderTalkgroups();
}

function actMatch(it){
  if (actKind && it.k !== actKind) return false;
  if (!actErr && it.crc === 0) return false;
  const q = $('act-q').value.trim().toLowerCase();
  if (!q) return true;
  return [it.src, it.dst, it.srcs, it.dsts, it.sn, it.dn, it.ty, it.sum, it.p, ...(it.f||[]).map(f=>f[1])].join(' ').toLowerCase().includes(q);
}
function hms(t){ return new Date(t*1000).toTimeString().slice(0,8); }
// m:ss (h:mm:ss past an hour): constant width while a call runs
function fmtClock(s){ s=Math.max(0, Math.round(s)); const h=Math.floor(s/3600), m=Math.floor(s/60)%60, x=String(s%60).padStart(2,'0');
  return h ? h+':'+String(m).padStart(2,'0')+':'+x : m+':'+x; }
function ago(s){ s=Math.max(0, Math.round(s)); return s<60 ? s+' s ago' : (s<3600 ? Math.floor(s/60)+' min ago' : Math.floor(s/3600)+' h ago'); }
// radio / talkgroup: callsign style id, or number with its name from the group list
function idHTML(num, str, name, prefix){
  if (str) return escH(str);
  if (num==null || num<0) return '';
  name = tgName(num) || name; // a name typed in the talkgroup list shows at once
  return (prefix||'')+escH(num)+(name?' <i>'+escH(name)+'</i>':'');
}

function actRow(it){
  const el=document.createElement('div');
  el.className='act k-'+it.k+(it.crc===0?' err':'')+(it.live?' live':'')+(actOpen.has(it.id)?' open':'');
  el.dataset.id=it.id; el.tabIndex=0;
  const who=idHTML(it.src, it.srcs, it.sn), to=idHTML(it.dst, it.dsts, it.dn, it.g===0 ? 'TG ' : '');
  let meta='';
  // a trunked grant: did the receiver go to the call?
  if (it.trk===1) meta+='<span class="tag ok" title="DSD-FME followed this call to its channel">Followed</span>';
  else if (it.trk===0) meta+='<span class="tag why" title="Not followed: '+escH(it.why||'')+'">Not followed: '+escH(it.why||'')+'</span>';
  if (it.enc) meta+='<span class="tag enc" title="'+escH(it.enc)+'">'+icon('lock')+escH(it.enc)+'</span>';
  if (it.lat!=null) meta+='<button class="tag lnk" data-lat="'+it.lat+'" data-lon="'+it.lon+'" title="'+it.lat.toFixed(5)+', '+it.lon.toFixed(5)+': show on the map" aria-label="Show on the map">'+icon('pin')+'</button>';
  if (it.rep>1) meta+='<span class="tag" title="Heard '+it.rep+' times (resent)">×'+it.rep+'</span>';
  if (it.ack) meta+='<span class="tag'+(it.ack==='delivered'?' ok':'')+'" title="'+escH(it.ack)+'">'+icon(it.ack==='delivered'?'check':'x')+'</span>';
  if (it.k==='call') meta+='<span class="dur" data-t="'+it.t+'">'+fmtClock(it.live ? Date.now()/1000-it.t : it.dur)+'</span>';
  el.innerHTML='<div class="l1"><span class="t">'+hms(it.t)+'</span><span class="ty">'+escH(it.ty||it.k)+'</span>'+
    (it.s?'<span class="sl">TS'+it.s+'</span>':'')+'<span class="who">'+who+(to?' → '+to:'')+'</span><span class="meta">'+meta+'</span></div>'+
    '<div class="sum">'+escH(it.sum)+'</div>';
  if (actOpen.has(it.id)) el.appendChild(actDetail(it));
  return el;
}

function hexDump(hex){
  const b=hex.match(/../g)||[]; let out='';
  for (let i=0;i<b.length;i+=16){
    const row=b.slice(i,i+16);
    const asc=row.map(h=>{ const c=parseInt(h,16); return c>=32&&c<127?String.fromCharCode(c):'.'; }).join('');
    out+=i.toString(16).padStart(4,'0')+'  '+row.join(' ').padEnd(47)+'  '+asc+'\n';
  }
  return out;
}
function actDetail(it){
  const d=document.createElement('div'); d.className='det';
  let kv='';
  const add=(k,v)=>{ kv+='<span class="k">'+escH(k)+'</span><span class="v">'+escH(v)+'</span>'; };
  add('Protocol', it.p+(it.s?', timeslot '+it.s:''));
  if (it.freq) add('Frequency', (it.freq/1e6).toFixed(5)+' MHz');
  if (it.k==='call') add('Time', hms(it.t)+(it.live?', in progress':' to '+hms(it.t+it.dur)));
  for (const [k,v] of it.f||[]) add(k,v);
  if (it.crc===0) add('Integrity','failed the CRC check');
  d.innerHTML='<div class="kv">'+kv+'</div>'+(it.hex?'<pre class="hexd">'+escH(hexDump(it.hex))+'</pre>':'');
  return d;
}
function actHint(){
  const box=$('acts');
  let h=box.querySelector('.acthint');
  if (box.querySelector('.act')){ if (h) h.remove(); return; }
  if (!h){ h=document.createElement('div'); h.className='acthint'; box.appendChild(h); }
  h.textContent = acts.size ? 'Nothing matches the filter.' : 'Calls, decoded data and signalling show up here as they are heard.';
}
// add, replace or drop one row, keeping the newest first
function actPut(it){
  const box=$('acts'), old=actEls.get(it.id);
  if (!actMatch(it)){ if (old){ old.remove(); actEls.delete(it.id); } actHint(); return; }
  const el=actRow(it);
  if (old) old.replaceWith(el);
  else {
    let next=null;
    for (const c of box.children) if (c.dataset.id && +c.dataset.id < it.id){ next=c; break; }
    box.insertBefore(el, next);
  }
  actEls.set(it.id, el);
  while (actEls.size > ACT_SHOW){ const last=[...box.children].reverse().find(c=>c.dataset.id); if (!last) break; actEls.delete(+last.dataset.id); last.remove(); }
  actHint();
}
function renderActs(){
  const box=$('acts'); box.innerHTML=''; actEls.clear();
  for (const it of [...acts.values()].filter(actMatch).sort((a,b)=>b.id-a.id).slice(0,ACT_SHOW)){ const el=actRow(it); box.appendChild(el); actEls.set(it.id, el); }
  actHint();
}
$('acts').addEventListener('click', e=>{
  const ln=e.target.closest('.lnk');
  if (ln){ const it=acts.get(+ln.closest('.act').dataset.id); mapOpen({lat:+ln.dataset.lat, lon:+ln.dataset.lon, id: it && (it.srcs || (it.src>0 ? String(it.src) : ''))}); return; }
  if (e.target.closest('a') || e.target.closest('.det')) return;
  const row=e.target.closest('.act'); if (!row) return;
  const id=+row.dataset.id, it=acts.get(id); if (!it) return;
  if (actOpen.has(id)) actOpen.delete(id); else actOpen.add(id);
  const had=document.activeElement===row;
  actPut(it);
  if (had && actEls.get(id)) actEls.get(id).focus(); // the row was rebuilt
});
// Enter or Space on a focused row opens / closes its details
$('acts').addEventListener('keydown', e=>{
  if ((e.key==='Enter' || e.key===' ') && e.target.classList.contains('act')){ e.preventDefault(); e.target.click(); }
});
$('act-kinds').addEventListener('click', e=>{
  const c=e.target.closest('.chip'); if (!c) return;
  const was=actKind; actKind=c.dataset.k;
  for (const x of $('act-kinds').children){ x.classList.toggle('on', x===c); x.setAttribute('aria-pressed', x===c); }
  const log=actKind==='log';
  $('acts').classList.toggle('hidden', log); $('events').classList.toggle('hidden', !log);
  if (log !== (was==='log')) subscribe();
  if (log){ $('events').innerHTML='<div class="acthint">Waiting for the decoder…</div>'; send({cmd:'get_status'}); }
  else renderActs();
});
$('act-q').oninput=()=>renderActs();
$('act-err').onclick=()=>{ actErr=!actErr; reflect('act-err', actErr); renderActs(); };
function clearAll(){ send({cmd:'activity_clear'}); send({cmd:'reset_events'}); }
$('act-clear').onclick=clearAll;

//---------------------------------------------------------------- radios
// every radio heard, from the activity: last activity, talkgroup, alias and last position
const radios = new Map();
function radioSeen(it, fresh){
  if (it.k==='sys') return;
  // the sender, or for messages to one radio from the system (P25 grants, acknowledgements) the radio addressed
  const key = it.srcs || (it.src>0 ? String(it.src) : (it.g===1 && it.dst>0 ? String(it.dst) : ''));
  if (!key) return;
  let r = radios.get(key);
  if (!r){ r={id:key, name:'', last:0}; radios.set(key, r); }
  const t = it.k==='call' && !it.live ? it.t+(it.dur||0) : it.t;
  if ((t >= r.last || it.id === r.item) && (it.crc !== 0 || !r.item)){ r.item=it.id; r.ty=it.ty; r.sum=it.sum; r.live=!!it.live; r.k=it.k; }
  r.last=Math.max(r.last, t);
  if (it.sn && key===String(it.src)) r.name=it.sn;
  else if (it.dn && key===String(it.dst)) r.name=it.dn;
  if (it.k==='call' && (it.dst>0 || it.dsts)) r.tg = it.dsts ? escH(it.dsts) : idHTML(it.dst, '', it.dn, it.g===1 ? 'radio ' : 'TG ');
  for (const [k,v] of it.f||[]) if (k==='Talker alias') r.alias=v;
  if (it.lat!=null && it.crc!==0 && it.t >= (r.pt||0)){ r.lat=it.lat; r.lon=it.lon; r.pt=it.t; }
  if (fresh && it.k!=='call' && it.crc!==0 && it.src>0) r.flash=Date.now()/1000+4; //it just sent something
}
let radT=0, radTimer=0;
function renderRadios(){
  if (radTimer) return;
  radTimer=setTimeout(()=>{ radTimer=0; radT=Date.now(); drawRadios(); }, Math.max(0, 600-(Date.now()-radT)));
}
function drawRadios(){
  const list=[...radios.values()].sort((a,b)=>b.last-a.last), now=Date.now()/1000;
  $('rad-count').textContent=list.length;
  let h='';
  for (const r of list.slice(0,300)){
    const air=onAir.has(r.id), tx=r.flash>now, nm=tgName(+r.id) || r.name;
    h+='<tr'+(air?' class="talk"':(tx?' class="tx"':''))+'>'+
      '<td title="'+escH(r.id+(nm?' '+nm:'')+(r.alias?' · '+r.alias:''))+'"><b>'+escH(r.id)+'</b>'+(nm?' <i>'+escH(nm)+'</i>':'')+(r.alias?' <i>'+escH(r.alias)+'</i>':'')+'</td>'+
      '<td>'+(air?'<span class="air">On air</span>':ago(now-r.last))+'</td>'+
      '<td title="'+escH(r.sum||'')+'"><span class="tag">'+escH(r.ty||'')+'</span> '+escH(r.sum||'')+'</td>'+
      '<td class="opt">'+(r.tg||'')+'</td>'+
      '<td>'+(r.lat!=null?'<button class="pos" data-id="'+escH(r.id)+'" title="'+r.lat.toFixed(5)+', '+r.lon.toFixed(5)+', '+ago(now-r.pt)+': show on the map" aria-label="Show '+escH(r.id)+' on the map">'+icon('pin')+'<span class="coords">'+r.lat.toFixed(4)+', '+r.lon.toFixed(4)+'</span></button>':'')+'</td></tr>';
  }
  if (!list.length) h='<tr><td colspan="5" class="empty">Radios show up here as they talk or send data.</td></tr>';
  // rebuilt every few seconds: keep keyboard focus on the same radio's position button
  const f=document.activeElement, fid=f && f.closest && f.closest('#rad-body') ? f.dataset.id : null;
  $('rad-body').innerHTML=h;
  if (fid){ const b=[...$('rad-body').querySelectorAll('[data-id]')].find(x=>x.dataset.id===fid); if (b) b.focus({preventScroll:true}); }
  if (MAP.open) mapDraw();
}
$('rad-body').addEventListener('click', e=>{
  const a=e.target.closest('[data-id]'); if (!a) return;
  const r=radios.get(a.dataset.id); if (r && r.lat!=null) mapOpen({lat:r.lat, lon:r.lon, id:r.id});
});

//---------------------------------------------------------------- trunking
// The card shows up once a control channel identifies itself (or while trunking): what the receiver
// does right now, and every talkgroup the system granted calls to, with a key that says whether
// DSD-FME follows it. The decoder's group list (modes A follow / B block) is what decides.
let trk = {}, trkDec = {}, trkFreq = 0, trkState = 'off', sysPic = null, trkFlags = {}, trkSlot = {};
let tgSummary = {voice:0, enc:0, data:0}; // calls heard on the system so far, from the talkgroup list
const groups = new Map(); // number -> {m: mode, n: name}
let tgKind = '', tgEdit = 0;  // list filter; number whose name is being edited

function groupsSet(m){
  groups.clear();
  for (const [id, mode, name] of m.items||[]) groups.set(id, {m:mode, n:name});
  renderTrunkNow(); drawTalkgroups(); renderActs(); renderRadios(); // names show everywhere
}
// as the server stores a name: no control characters or commas (the CSV export), single spaces
function cleanName(s){ return s.replace(/[\x00-\x1f\x7f,]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 60); }
function tgName(id){ const g=groups.get(id); return g && g.n || ''; }
// lit: followed as far as the group list and the allow list go (the call type keys and holds aside)
function tgLit(id){ const g=groups.get(id); if (!g || !g.m) return !trk.allow_list; return g.m!=='B' && g.m!=='DE'; }
function trunkable(s){
  if (!s) return false;
  const p=s.p25, d=s.dmr, n=s.nxdn;
  return !!((p && (p.cc || p.cc_ch)) || (d && (d.syscode || d.rest > 0 || /cap|con|xpt/i.test(d.sub||''))) || (n && (n.sys || n.site)));
}
function sysLine(s){
  if (s && s.p25){ const p=s.p25;
    return ['P25', p.wacn && p.sys ? p.wacn+'-'+p.sys : (p.sys ? 'SYS '+p.sys : ''), p.rfss!=null ? 'RFSS '+p.rfss+' site '+(p.site!=null ? p.site : '?') : '',
      p.cc ? 'control channel '+(p.cc/1e6).toFixed(5)+' MHz' : ''].filter(x=>x).join(' · '); }
  if (s && s.dmr){ const d=s.dmr;
    return ['DMR', [d.brand, d.sub].filter(x=>x).join(' ').trim(), d.syscode ? 'system '+d.syscode : '', d.rest > 0 ? 'rest channel '+d.rest : ''].filter(x=>x).join(' · '); }
  if (s && s.nxdn){ const n=s.nxdn; return ['NXDN', n.sys ? 'system '+n.sys : '', n.site ? 'site '+n.site : ''].filter(x=>x).join(' · '); }
  return '';
}
const KIND_WORD = {'-1':'call', 0:'group call', 1:'private call', 2:'data', 3:'phone call'};
function idWithName(id){ const n=tgName(id); return escH(id)+(n ? ' <i>'+escH(n)+'</i>' : ''); }
// a followed call as the card names it: "TG 2098 Police", "data for radio 732216"
function callWho(c){
  if (!(c.id > 0)) return 'a call';
  const pre = c.k===2 ? 'data for radio ' : (c.k===1 ? 'call to radio ' : (c.k===3 ? 'phone call, radio ' : 'TG '));
  return pre+idWithName(c.id);
}

// P25 packet traffic: answers to confirmed packets (signalling) and the packets themselves
const PKT_SIG = new Set(['ACK','SACK','NACK','Response']);
function isPkt(it){ return it.p==='P25' && (it.k==='data' || (it.k==='sig' && PKT_SIG.has(it.ty))); }
const PKT_WORD = {ACK:'delivered', SACK:'resend asked', NACK:'not delivered', 'Encrypted data':'encrypted data', 'Data error':'damaged packet'};
// one packet as the card lists it: "radio 140000: Triggered location start request"
function pktLine(it){
  const id = it.dst > 0 ? it.dst : it.src, what = PKT_WORD[it.ty] || (it.sum ? it.sum : it.ty);
  return (id > 0 ? 'radio '+idWithName(id)+': ' : '')+escH(what.length > 48 ? what.slice(0, 46)+'…' : what);
}
function renderTrunk(m){
  trk = m.trunk||{}; trkDec = m.decode||{}; trkFreq = (m.input||{}).freq||0; trkFlags = m.flags||{}; trkSlot = m.slot1||{};
  const tr=trk;
  // off a call, the receiver only leaves the control channel to hunt for it
  const away = trkFreq > 0 && tr.cc_freq > 0 && Math.abs(trkFreq - tr.cc_freq) > 3000;
  trkState = !tr.enabled ? 'off' : (!tr.cc_freq ? 'wait' : (tr.tuned ? (trkDec.carrier ? 'follow' : 'hang') :
    (away || (!trkDec.carrier && tr.quiet > (tr.hangtime||0)+1) ? 'hunt' : 'cc')));
  reflect('tk-grp', tr.group); reflect('tk-prv', tr.private); reflect('tk-data', tr.data); reflect('tk-enc', tr.enc);
  reflect('tk-allow', tr.allow_list);
  if (document.activeElement !== $('tk-hang') && tr.hangtime !== undefined) $('tk-hang').value = tr.hangtime;
  renderTrunkNow();
}

const TRK_BADGE = {off:['Off',''], wait:['Waiting','warn'], cc:['Watching','live'], follow:['Following','live'], hang:['Returning','warn'], hunt:['Searching','off']};
// spectrum marks for the control channel and the channels of calls on air
let trkMarks = [];
function trunkMarks(){
  trkMarks = [];
  if (!(trk.enabled || trunkable(sysPic))) return;
  const cc = trk.cc_freq || (sysPic && sysPic.p25 && sysPic.p25.cc);
  if (cc) trkMarks.push({freq:cc, name:'Control channel', k:'cc'});
  const f=trk.follow||{}, seen=new Set([cc]);
  for (const it of acts.values()){
    if (it.k!=='call' || it.p!=='P25' || !it.live || !(it.freq > 0) || seen.has(it.freq)) continue;
    seen.add(it.freq);
    const name = it.g===0 ? 'TG '+it.dst+(tgName(it.dst) ? ' '+tgName(it.dst) : '') : '→ '+it.dst;
    trkMarks.push({freq:it.freq, name, k: trkState==='follow' && f.f===it.freq ? 'fol' : 'air'});
  }
}
function renderTrunkNow(){
  trunkMarks();
  const tr=trk, st=trkState, f=tr.follow||{};
  $('card-trk').classList.toggle('hidden', !(tr.enabled || trunkable(sysPic)));
  const [bt, bc] = TRK_BADGE[st];
  setTxt($('trk-badge'), bt); $('trk-badge').className = 'badge '+bc;
  const lamp=$('st-trk');
  setTxt(lamp, st==='follow' && f.id > 0 ? (f.k===0 || f.k===-1 ? 'TG ' : 'Radio ')+f.id : 'Trunking', 'Trunking: '+bt.toLowerCase());
  lamp.className = 'badge '+bc+(tr.enabled ? '' : ' hidden');
  setTxt($('trk-sys'), sysLine(sysPic) || (tr.cc_freq ? 'Control channel '+(tr.cc_freq/1e6).toFixed(5)+' MHz' : ''));
  let what='', sub='';
  const show = {go:false, skip:false, back:false, hold:false, block:false, stop:!!tr.enabled};
  if (st==='off'){
    what = 'Not following calls';
    sub = tr.can===false ? 'Trunking needs a receiver DSD-FME can retune: start it with <code>-i rtl</code>, or with rigctl (<code>-U</code>).'
      : 'Follow calls: the receiver jumps to each call’s channel and comes back here when the call ends.';
    show.go = true;
  }
  else if (st==='wait'){
    what = 'Waiting for the control channel';
    sub = 'It identifies itself within a few seconds. DMR and NXDN systems that send channel numbers need a channel map (Settings → Trunk).';
  }
  else if (st==='follow' || st==='hang'){
    if (st==='follow'){
      what = 'Following '+callWho(f);
      const bits=[];
      if (f.f) bits.push('<b>'+(f.f/1e6).toFixed(5)+' MHz</b>'+(f.ch ? ' (ch '+escH(f.ch)+')' : ''));
      bits.push(f.k===2 ? 'packet data, nothing to hear' : (KIND_WORD[f.k] || 'call'));
      if (f.src > 0) bits.push('radio '+idWithName(f.src));
      // the voice channel names the cipher; without its key the decoder keeps the audio muted
      const enc = (trkSlot.tg || trkSlot.src ? (trkSlot.enc||'').trim() : '') || (f.enc ? 'encrypted' : ''); // the slot keeps it after a call
      if (enc) bits.push(escH(enc)+(trkFlags.key_loaded ? '' : ', muted without its key'));
      if (f.em) bits.push('<b>emergency</b>');
      bits.push(fmtClock(f.dur||0));
      sub = bits.join(' · ');
      // a data channel is shared: everything it carried since the receiver got there, newest first
      const since = Date.now()/1000 - (f.dur||0) - 1, pk = [];
      if (f.k===2) for (const it of acts.values()) if (isPkt(it) && it.t >= since) pk.push(it);
      pk.sort((a,b)=>b.t-a.t);
      if (pk.length) sub += '<br>'+pk.length+' packet'+(pk.length===1?'':'s')+' here: '+pk.slice(0,3).map(pktLine).join(' · ')+(pk.length > 3 ? ' …' : '');
      else if (f.k===2 && tr.data) sub += '<br>Data calls carry no audio. To wait for voice calls instead, switch off Data under Follow.';
      else if (f.k!==2 && !enc && !audioOn) sub += '<br>The audio plays on the DSD-FME computer. To hear it in this browser, press Audio off at the top.';
      show.skip = !(tr.tg_hold && tr.tg_hold===f.id);
    } else {
      what = 'Call ended';
      sub = 'Back to the control channel in '+Math.max(0, Math.ceil((tr.hangtime||0) - (tr.quiet||0)))+' s';
      show.back = true;
    }
    show.hold = f.id > 0; show.block = f.id > 0 && tr.tg_hold!==f.id;
  }
  else if (st==='hunt'){
    const h=tr.hunt||[], i=h.indexOf(trkFreq);
    what = 'Control channel lost';
    sub = 'Searching: trying <b>'+(trkFreq/1e6).toFixed(5)+' MHz</b>'+(i>=0 ? ' ('+(i+1)+' of '+h.length+')' : '')+
      '. A site moves its control channel to a backup when the main one fails.';
  }
  else {
    what = 'Watching the control channel';
    const l=tr.last;
    sub = (l ? 'Last call: '+callWho(l)+', '+fmtClock(l.dur)+', '+ago(l.ago) : 'No call followed yet')+
      ' · '+(tr.nf||0)+' followed, '+(tr.ns||0)+' not followed';
    const g=tgSummary;
    if (g.voice || g.data) sub += '<br>Heard so far: '+g.voice+' voice call'+(g.voice===1?'':'s')+
      (g.enc ? ' ('+(g.enc===g.voice ? (g.voice===1 ? 'encrypted' : 'all encrypted') : g.enc+' encrypted')+')' : '')+', '+g.data+' data';
    if (tr.allow_list && ![...groups.values()].some(g => g.m && g.m!=='B' && g.m!=='DE'))
      sub += '<br>Only lit talkgroups are followed, and none is lit yet: light the ones to follow below.';
  }
  if (tr.enabled && tr.tg_hold){ sub += '<br>Holding '+holdWho(tr.tg_hold)+': calls to anything else are not followed.'; show.hold = true; }
  const skipping = (tr.skip||[]).filter(id => id !== f.id);
  if (tr.enabled && skipping.length) sub += '<br>Skipped until their calls end: '+skipping.map(idWithName).join(', ');
  setHTML($('trk-what'), what); setHTML($('trk-sub'), sub);
  $('trk-now').classList.toggle('on', st==='follow');
  for (const k in show) $('trk-'+k).classList.toggle('hidden', !show[k]);
  $('trk-go').disabled = tr.can===false;
  setTxt($('trk-hold'), tr.tg_hold ? 'Release hold' : (f.k===0 || f.k===-1 ? 'Hold TG' : 'Hold radio'));
}
// a held number is a talkgroup unless the list only knows it as a radio
const tgKinds = new Map();
function holdWho(id){ const k=tgKinds.get(id); return (k && k!=='grp' ? 'radio ' : 'TG ')+idWithName(id); }

// the talkgroup list, rebuilt from the activity: grants on the control channel (this system's), and
// calls heard on voice channels while trunking
const TG_KIND = {'Group call grant':'grp', 'Private call grant':'prv', 'Data channel grant':'data', 'Phone call grant':'tel'};
const TG_WORD = {grp:'Group', prv:'Private', data:'Data', tel:'Phone'};
function grantSys(it){ for (const [k,v] of it.f||[]) if (k==='System'){ const m=/WACN (\w+), SYS (\w+)/.exec(v); return m ? m[1]+'-'+m[2] : ''; } return ''; }
function tgBuild(){
  const p=sysPic && sysPic.p25, cur = p && p.wacn && p.sys ? p.wacn+'-'+p.sys : '';
  const list=new Map(), now=Date.now()/1000;
  for (const it of [...acts.values()].sort((a,b)=>a.t-b.t)){
    if (it.k!=='call' || !(it.dst > 0)) continue;
    const grant = it.p==='P25' && TG_KIND[it.ty];
    if (grant){ const s=grantSys(it); if (cur && s && s!==cur) continue; }
    else if (!it.trkd) continue;
    let r=list.get(it.dst);
    if (!r){ r={id:it.dst, calls:0, end:0}; list.set(it.dst, r); }
    const end = it.live ? now : it.t+(it.dur||0);
    if (it.t > r.end + 5){ r.calls++; r.epEnc = false; } // a call is the grants and voice of one stretch of activity
    if (it.enc && !r.epEnc){ r.epEnc = true; r.encCalls = (r.encCalls||0)+1; }
    r.end = Math.max(r.end, end);
    r.kind = grant ? TG_KIND[it.ty] : (it.g===1 ? 'prv' : 'grp');
    r.live = r.live || !!it.live;
    if (grant && it.trk !== undefined){ r.trk=it.trk; r.why=it.why||''; r.dur = it.live ? now-it.t : it.dur; }
  }
  // packets to or from the radios in the list (heard on data channels while following)
  for (const it of acts.values()){
    if (!isPkt(it)) continue;
    const r = list.get(it.dst > 0 ? it.dst : it.src); if (!r) continue;
    r.pk = (r.pk||0) + 1;
    const w = PKT_TALLY[it.ty] || it.ty.toLowerCase();
    (r.pkc = r.pkc || {})[w] = (r.pkc[w]||0) + (it.rep||1);
  }
  return list;
}
// what a radio's packets came to, most first: "3 delivered · 1 resend"
const PKT_TALLY = {ACK:'delivered', SACK:'resend', NACK:'not delivered', LRRP:'location', ARS:'registration', SNDCP:'session',
  'Encrypted data':'encrypted', Location:'position', 'Data error':'damaged'};
function pktTally(r){ return Object.entries(r.pkc||{}).sort((a,b)=>b[1]-a[1]).slice(0,3).map(([w,n])=>n+' '+w).join(' · '); }
let tgT=0, tgTimer=0;
function renderTalkgroups(){
  if (tgTimer) return;
  tgTimer=setTimeout(()=>{ tgTimer=0; tgT=Date.now(); drawTalkgroups(); }, Math.max(0, 700-(Date.now()-tgT)));
}
function drawTalkgroups(){
  if (tgEdit) return; // a name being typed: the list waits
  const now=Date.now()/1000, all=[...tgBuild().values()];
  tgKinds.clear(); for (const r of all) tgKinds.set(r.id, r.kind);
  tgSummary = {voice:0, enc:0, data:0};
  for (const r of all){ if (r.kind==='data') tgSummary.data += r.calls; else { tgSummary.voice += r.calls; tgSummary.enc += r.encCalls||0; } }
  const list=all.filter(r => !tgKind || (tgKind==='grp') === (r.kind==='grp')).sort((a,b)=>(b.live-a.live) || (b.end-a.end));
  $('tg-count').textContent = list.length===all.length ? all.length : list.length+' of '+all.length;
  const f=trk.follow||{};
  let h='';
  // a call type switched off above leaves its rows' keys dark and out of reach
  const typeOff = {grp:!trk.group, prv:!trk.private, tel:!trk.private, data:!trk.data};
  const typeName = {grp:'Group calls', prv:'Private calls', tel:'Private calls', data:'Data calls'};
  for (const r of list.slice(0,300)){
    const off=typeOff[r.kind], lit=!off && tgLit(r.id), held=trk.tg_hold===r.id, name=tgName(r.id), here=trkState==='follow' && f.id===r.id;
    let out;
    if (here) out='<span class="fol">Listening</span> · '+fmtClock(f.dur||0);
    else if (r.trk===1) out='<span class="fol">Followed</span>'+(r.dur ? ' · '+fmtClock(r.dur) : '');
    else if (r.trk===0) out=(r.live ? 'On air, not followed: ' : 'Not followed: ')+escH(r.why);
    else out=r.live ? 'On air' : 'Heard';
    // a data radio's packets say more than its follow: they were only heard because it was followed
    let tip = out.replace(/<[^>]+>/g,'');
    if (r.pk && !here){ out=escH(pktTally(r)); tip += ' · '+r.pk+' packet'+(r.pk===1?'':'s')+': '+out; }
    const what=(r.kind==='grp' ? 'TG ' : 'radio ')+r.id;
    h+='<tr data-id="'+r.id+'"'+(here||held ? ' class="'+(here?'live':'')+(held?' held':'')+'"' : '')+'>'+
      '<td><button class="tgl sm led'+(lit?' on':'')+'" data-act="led" aria-pressed="'+lit+'" aria-label="Follow '+what+'"'+(off ? ' aria-disabled="true"' : '')+' title="'+
        (off ? typeName[r.kind]+' are off: switch them on under Follow to follow '+what : (lit ? 'Followed: click to stop following '+what : 'Not followed: click to follow '+what))+'"></button></td>'+
      '<td><button class="num" data-act="find" title="Show everything about '+what+' in Activity">'+r.id+'</button><button class="nm'+(name?'':' add')+'" data-act="name" title="Name '+what+'">'+(name ? escH(name) : 'name')+'</button></td>'+
      '<td class="opt"'+(r.kind==='data' ? ' title="Packet data for this radio: nothing to hear"' : '')+'>'+TG_WORD[r.kind]+'</td>'+
      '<td class="opt2">'+(r.live ? '<span class="air">On air</span>' : ago(now-r.end))+'</td>'+
      '<td class="opt">'+r.calls+'</td>'+
      '<td class="out" title="'+escH(tip)+'">'+out+'</td>'+
      '<td><button class="sm hold" data-act="hold" title="'+(held ? 'Stop holding '+what : 'Follow only '+what)+'">'+(held ? 'Release' : 'Hold')+'</button></td></tr>';
  }
  if (!all.length) h='<tr><td colspan="7" class="empty">Talkgroups show up here as the control channel grants calls.</td></tr>';
  else if (!list.length) h='<tr><td colspan="7" class="empty">None of this kind yet.</td></tr>';
  // rebuilt every second or so: keep keyboard focus on the same key, without scrolling to it (a key
  // tapped on a phone keeps the focus, and the page would jump back to it on every rebuild)
  const fo=document.activeElement, fr=fo && fo.closest && fo.closest('#tg-body tr'), fid=fr ? fr.dataset.id : null, fact=fo && fo.dataset ? fo.dataset.act : null;
  $('tg-body').innerHTML=h;
  if (fid){ const b=$('tg-body').querySelector('tr[data-id="'+fid+'"] [data-act="'+fact+'"]'); if (b) b.focus({preventScroll:true}); }
}
function tgNameEdit(id){
  const row=$('tg-body').querySelector('tr[data-id="'+id+'"]'); if (!row) return;
  tgEdit=id;
  const b=row.querySelector('[data-act="name"]'), inp=document.createElement('input');
  inp.className='nmed'; inp.maxLength=60; inp.value=tgName(id); inp.setAttribute('aria-label', 'Name for '+id);
  inp.placeholder='name';
  b.replaceWith(inp); inp.focus(); inp.select();
  let done=false;
  const finish=(save)=>{ if (done) return; done=true; tgEdit=0;
    if (save){ const v=cleanName(inp.value); const g=groups.get(id);
      if (v !== tgName(id)){ send({cmd:'tg_name', id, name:v}); groups.set(id, {m:g ? g.m : '', n:v}); renderActs(); } }
    drawTalkgroups();
    const nb=$('tg-body').querySelector('tr[data-id="'+id+'"] [data-act="name"]'); if (nb) nb.focus({preventScroll:true}); };
  inp.addEventListener('keydown', e=>{ if (e.key==='Enter'){ e.preventDefault(); finish(true); } else if (e.key==='Escape'){ e.preventDefault(); finish(false); } });
  inp.addEventListener('blur', ()=>finish(true));
}
$('tg-body').addEventListener('click', e=>{
  const b=e.target.closest('[data-act]'); if (!b) return;
  const id=+b.closest('tr').dataset.id;
  if (b.dataset.act==='led'){
    if (b.getAttribute('aria-disabled')==='true') return; // its call type is off (the title says so)
    const mode = b.classList.contains('on') ? 'B' : 'A';
    send({cmd:'tg_mode', id, mode}); groups.set(id, {m:mode, n:tgName(id)}); drawTalkgroups();
  }
  else if (b.dataset.act==='name') tgNameEdit(id);
  else if (b.dataset.act==='find'){ // Activity, filtered to this number, all kinds
    $('act-q').value=String(id);
    const all=$('act-kinds').querySelector('[data-k=""]'); if (actKind) all.click(); else renderActs();
    document.querySelector('.card-act').scrollIntoView({block:'start'});
  }
  else if (b.dataset.act==='hold') send({cmd:'tg_hold', tg: trk.tg_hold===id ? 0 : id});
});
$('tg-kinds').addEventListener('click', e=>{
  const c=e.target.closest('.chip'); if (!c) return;
  tgKind=c.dataset.k;
  for (const x of $('tg-kinds').children){ x.classList.toggle('on', x===c); x.setAttribute('aria-pressed', x===c); }
  drawTalkgroups();
});
$('trk-go').onclick=()=>send({cmd:'trunk_start'});
$('trk-stop').onclick=()=>send({cmd:'trunk_stop'});
$('trk-skip').onclick=()=>send({cmd:'trunk_skip'});
$('trk-back').onclick=()=>send({cmd:'return_cc'});
$('trk-hold').onclick=()=>{ const f=trk.follow||{}; send({cmd:'tg_hold', tg: trk.tg_hold ? 0 : (f.id||0)}); };
// block the call being followed and leave it now
$('trk-block').onclick=()=>{ const f=trk.follow||{}; if (!(f.id > 0)) return;
  send({cmd:'tg_mode', id:f.id, mode:'B'}); groups.set(f.id, {m:'B', n:tgName(f.id)}); send({cmd:'return_cc'}); };
for (const [id,key] of [['tk-grp','group'],['tk-prv','private'],['tk-data','data'],['tk-enc','enc'],['tk-allow','allow_list']])
  $(id).onclick=()=>{ const on=!$(id).classList.contains('on'); const o={cmd:'trunk_opts'}; o[key]=on; send(o); reflect(id, on); if (key==='allow_list'){ trk.allow_list=on; drawTalkgroups(); } };
$('tk-hang').onchange=()=>{ const v=+$('tk-hang').value; if (v >= 0) send({cmd:'hangtime', seconds:v}); };
setInterval(()=>{ if (!$('card-trk').classList.contains('hidden')) renderTalkgroups(); }, 1000);

//---------------------------------------------------------------- map (modal)
// OpenStreetMap tiles when the browser is online; offline the dots still sit right on a dark
// background with a scale bar. Radios on air pulse red and come into view, data makes a radio flash.
// z can be fractional (pinch, wheel): tiles come from the nearest whole zoom level, scaled
const MAP = {open:false, z:13, lat:0, lon:0, tiles:new Map(), fitted:false, focus:null, sel:null, follow:true, air:new Set(), drag:null, pinch:null, back:null, raf:0};
const TS = 256;
function mpx(lon, z){ return (lon+180)/360*TS*2**z; }
function mpy(lat, z){ const s=Math.sin(Math.max(-85.05, Math.min(85.05, lat))*Math.PI/180); return (0.5-Math.log((1+s)/(1-s))/(4*Math.PI))*TS*2**z; }
function mlon(x, z){ return x/(TS*2**z)*360-180; }
function mlat(y, z){ const n=Math.PI-2*Math.PI*y/(TS*2**z); return 180/Math.PI*Math.atan(Math.sinh(n)); }
function mapPts(){ return [...radios.values()].filter(r=>r.lat!=null); }
function airLabel(id){ const c=onAir.get(id); return !c ? '' : (c.dsts ? c.dsts : (c.dst>0 ? (c.g===1 ? 'to '+c.dst : 'TG '+c.dst) : 'on air')); }

function mapOpen(focus){
  if (!MAP.open){ MAP.back=document.activeElement; MAP.open=true; $('mapdlg').classList.remove('hidden'); pageInert(true); }
  MAP.focus = focus && focus.lat!=null ? {lat:focus.lat, lon:focus.lon} : null;
  if (focus && focus.id) MAP.sel=focus.id;
  if (MAP.focus){ MAP.lat=focus.lat; MAP.lon=focus.lon; MAP.z=Math.max(MAP.z, 14); MAP.fitted=true; }
  else if (!MAP.fitted) mapFit();
  mapDraw();
  $('map-close').focus();
}
function mapClose(){
  MAP.open=false; MAP.focus=null; $('mapdlg').classList.add('hidden');
  pageInert(!$('login').classList.contains('hidden'));
  if (MAP.back && MAP.back.focus) MAP.back.focus();
}
function mapFit(){
  const pts=mapPts(), v=$('mapview'); if (!pts.length) return;
  const w=v.clientWidth||900, h=v.clientHeight||600;
  let la0=90, la1=-90, lo0=180, lo1=-180;
  for (const p of pts){ la0=Math.min(la0,p.lat); la1=Math.max(la1,p.lat); lo0=Math.min(lo0,p.lon); lo1=Math.max(lo1,p.lon); }
  let z=16;
  while (z>2 && (mpx(lo1,z)-mpx(lo0,z) > w-90 || mpy(la0,z)-mpy(la1,z) > h-90)) z--;
  MAP.z=z; MAP.lon=(lo0+lo1)/2; MAP.lat=mlat((mpy(la0,z)+mpy(la1,z))/2, z); MAP.fitted=true;
}
// a radio with a position that just went on air comes into view (while following)
function mapFollow(w, h){
  const now=new Set();
  for (const r of mapPts()) if (onAir.has(r.id)) now.add(r.id);
  for (const id of now){
    if (MAP.air.has(id) || !MAP.follow) continue;
    const r=radios.get(id), x=mpx(r.lon,MAP.z)-mpx(MAP.lon,MAP.z)+w/2, y=mpy(r.lat,MAP.z)-mpy(MAP.lat,MAP.z)+h/2;
    if (x<40 || y<40 || x>w-40 || y>h-40){ MAP.lat=r.lat; MAP.lon=r.lon; MAP.focus=null; }
  }
  MAP.air=now;
}
function mapDraw(){
  if (!MAP.open) return;
  const v=$('mapview'), w=v.clientWidth, h=v.clientHeight; if (!w || !h) return;
  if (!MAP.drag && !MAP.pinch) mapFollow(w, h);
  const z=MAP.z, tz=Math.round(z), n=2**tz, ts=TS*2**(z-tz), x0=mpx(MAP.lon,z)-w/2, y0=mpy(MAP.lat,z)-h/2, keep=new Set();
  for (let tx=Math.floor(x0/ts); tx*ts<x0+w; tx++) for (let ty=Math.max(0, Math.floor(y0/ts)); ty*ts<y0+h && ty<n; ty++){
    const key=tz+'/'+tx+'/'+ty; keep.add(key);
    let img=MAP.tiles.get(key);
    if (!img){
      img=document.createElement('img'); img.className='tile'; img.alt=''; img.draggable=false;
      img.onerror=()=>img.classList.add('miss');
      img.src='https://tile.openstreetmap.org/'+tz+'/'+(((tx%n)+n)%n)+'/'+ty+'.png';
      $('maptiles').appendChild(img); MAP.tiles.set(key, img);
    }
    // edges rounded on their own so scaled tiles meet without seams
    const l=Math.round(tx*ts-x0), t=Math.round(ty*ts-y0);
    img.style.transform='translate('+l+'px,'+t+'px)';
    img.style.width=(Math.round((tx+1)*ts-x0)-l)+'px'; img.style.height=(Math.round((ty+1)*ts-y0)-t)+'px';
  }
  for (const [k, img] of MAP.tiles) if (!keep.has(k)){ img.remove(); MAP.tiles.delete(k); }
  const now=Date.now()/1000, pts=[];
  for (const r of mapPts()){
    const x=Math.round(mpx(r.lon,z)-x0), y=Math.round(mpy(r.lat,z)-y0);
    if (x>=-80 && y>=-30 && x<=w+80 && y<=h+30) pts.push({r, x, y, air:airLabel(r.id)});
  }
  //when crowded, labels go to the selected radio, radios on air, then the newest positions
  const boxes=[];
  for (const p of [...pts].sort((a,b)=>(b.r.id===MAP.sel)-(a.r.id===MAP.sel) || !!b.air-!!a.air || b.r.pt-a.r.pt)){
    const len=p.r.id.length+(p.air ? p.air.length+3 : 0), bx={x0:p.x+6, y0:p.y-9, x1:p.x+10+len*6.8, y1:p.y+6};
    p.label = !boxes.some(o=>bx.x0<o.x1 && bx.x1>o.x0 && bx.y0<o.y1 && bx.y1>o.y0);
    if (p.label) boxes.push(bx);
  }
  pts.sort((a,b)=>!!a.air-!!b.air || a.r.pt-b.r.pt); //on air drawn last, on top
  let mk='';
  for (const {r, x, y, air, label} of pts){
    const age=now-r.pt;
    mk+='<div class="mk'+(air?' air':'')+(label?'':' nl')+(r.flash>now?' tx':'')+(age<180?' fresh':(age<1200?' recent':''))+(MAP.sel===r.id?' sel':'')+
      '" data-id="'+escH(r.id)+'" style="transform:translate('+x+'px,'+y+'px)" title="'+
      escH(r.id+(r.name?' '+r.name:'')+(air?' · on air '+air:'')+' · '+(r.ty||'')+' '+(r.sum||'')+' · position from '+ago(age))+
      '"><i></i><span>'+escH(r.id)+(air?' · '+escH(air):'')+'</span></div>';
  }
  if (MAP.focus) mk+='<div class="mk focus" style="transform:translate('+Math.round(mpx(MAP.focus.lon,z)-x0)+'px,'+Math.round(mpy(MAP.focus.lat,z)-y0)+'px)"><i></i></div>';
  $('mapmarks').innerHTML=mk;
  $('mapempty').classList.toggle('hidden', mapPts().length>0 || !!MAP.focus);
  // scale bar: a round distance close to a sixth of the width
  const mpp=156543.03392*Math.cos(MAP.lat*Math.PI/180)/2**z; //metres per pixel
  let d=5; for (const sd of [10,20,50,100,200,500,1000,2000,5000,10000,20000,50000,100000,200000,500000,1000000]) if (sd/mpp < w/6) d=sd;
  $('mapscale').style.width=Math.round(d/mpp)+'px';
  $('mapscale').textContent=d>=1000 ? d/1000+' km' : d+' m';
  $('map-osm').href='https://www.openstreetmap.org/#map='+tz+'/'+MAP.lat.toFixed(5)+'/'+MAP.lon.toFixed(5);
  mapSide(now);
}
function mapSide(now){
  const air=[...onAir.entries()].sort((a,b)=>a[1].t-b[1].t);
  setTxt($('map-onair'), air.length ? air.length+' on air' : 'Nobody on air');
  $('map-onair').className='badge'+(air.length?' live':'');
  let h='<h4>On air</h4>';
  if (!air.length) h+='<div class="hint">Nobody is transmitting.</div>';
  for (const [id, c] of air){
    const r=radios.get(id), pos=r && r.lat!=null;
    h+='<button class="it air" data-id="'+escH(id)+'" title="'+(pos?'Show on the map':'No position from this radio yet')+'"><span>'+escH(id)+'</span><span class="x">'+escH(airLabel(id))+', '+fmtClock(now-c.t)+(pos?'':', no position')+'</span></button>';
  }
  h+='<h4>Recently heard</h4>';
  const rec=mapPts().sort((a,b)=>b.last-a.last).slice(0,40);
  if (!rec.length) h+='<div class="hint">No positions yet.</div>';
  for (const r of rec) h+='<button class="it'+(MAP.sel===r.id?' sel':'')+'" data-id="'+escH(r.id)+'"><span>'+escH(r.id)+(r.name?' '+escH(r.name):'')+'</span><span class="x">'+escH(r.ty||'')+', '+ago(now-r.last)+'</span></button>';
  // rebuilt every second while someone talks: keep keyboard focus on the same radio
  const f=document.activeElement, fid=f && f.classList.contains('it') ? f.dataset.id : null;
  setHTML($('mapside'), h);
  if (fid && document.activeElement!==f){ const b=[...$('mapside').querySelectorAll('.it')].find(x=>x.dataset.id===fid); if (b) b.focus({preventScroll:true}); }
}
function mapZoom(dz, cx, cy){
  const v=$('mapview'), w=v.clientWidth, h=v.clientHeight, z=MAP.z, nz=Math.max(2, Math.min(19, z+dz));
  if (nz===z) return;
  if (cx===undefined){ cx=w/2; cy=h/2; }
  //the point under the cursor stays where it is
  const lon=mlon(mpx(MAP.lon,z)-w/2+cx, z), lat=mlat(mpy(MAP.lat,z)-h/2+cy, z);
  MAP.z=nz; MAP.lon=mlon(mpx(lon,nz)-cx+w/2, nz); MAP.lat=mlat(mpy(lat,nz)-cy+h/2, nz);
  mapDrawSoon();
}
// pointer and wheel events can come faster than frames
function mapDrawSoon(){ if (!MAP.raf) MAP.raf=requestAnimationFrame(()=>{ MAP.raf=0; mapDraw(); }); }
$('btn-map').onclick=()=>mapOpen();
$('rad-map').onclick=()=>mapOpen();
$('map-close').onclick=mapClose;
$('mapdlg').addEventListener('click', e=>{ if (e.target===$('mapdlg')) mapClose(); });
document.addEventListener('keydown', e=>{ if (e.key==='Escape' && MAP.open) mapClose(); });
$('map-fit').onclick=()=>{ MAP.focus=null; mapFit(); mapDraw(); };
$('map-follow').onclick=()=>{ MAP.follow=!MAP.follow; reflect('map-follow', MAP.follow); };
// the buttons step to the next whole zoom level, where tiles are sharpest
$('map-zin').onclick=()=>mapZoom(Math.floor(MAP.z+1e-6)+1-MAP.z);
$('map-zout').onclick=()=>mapZoom(Math.ceil(MAP.z-1e-6)-1-MAP.z);
// a mouse wheel notch is one zoom level; trackpads send many small deltas, which zoom smoothly
$('mapview').addEventListener('wheel', e=>{
  e.preventDefault(); const r=$('mapview').getBoundingClientRect();
  const px=e.deltaMode===1 ? e.deltaY*33 : (e.deltaMode===2 ? e.deltaY*400 : e.deltaY);
  mapZoom(Math.max(-1, Math.min(1, -px/100)), e.clientX-r.left, e.clientY-r.top);
}, {passive:false});
// one pointer drags the map (or taps a radio); two pinch: zoom about the point between the
// fingers, which also pans with them
const mapPtrs = new Map();
function mapPinchFrom(){
  const [a, b]=[...mapPtrs.values()], r=$('mapview').getBoundingClientRect();
  return {d:Math.max(1, Math.hypot(a.x-b.x, a.y-b.y)), cx:(a.x+b.x)/2-r.left, cy:(a.y+b.y)/2-r.top, w:r.width, h:r.height};
}
$('mapview').addEventListener('pointerdown', e=>{
  mapPtrs.set(e.pointerId, {x:e.clientX, y:e.clientY});
  $('mapview').setPointerCapture(e.pointerId); $('mapview').classList.add('drag');
  if (mapPtrs.size===2){
    const p=mapPinchFrom();
    MAP.pinch={d:p.d, z:MAP.z, lon:mlon(mpx(MAP.lon,MAP.z)-p.w/2+p.cx, MAP.z), lat:mlat(mpy(MAP.lat,MAP.z)-p.h/2+p.cy, MAP.z)};
    MAP.drag=null; // a pinch is not a tap on a radio
  }
  else if (mapPtrs.size===1){
    const m=e.target.closest('.mk');
    MAP.drag={x:e.clientX, y:e.clientY, px:mpx(MAP.lon,MAP.z), py:mpy(MAP.lat,MAP.z), moved:false, id:m && m.dataset.id};
  }
});
$('mapview').addEventListener('pointermove', e=>{
  if (!mapPtrs.has(e.pointerId)) return;
  mapPtrs.set(e.pointerId, {x:e.clientX, y:e.clientY});
  if (MAP.pinch && mapPtrs.size>=2){
    const p=mapPinchFrom(), P=MAP.pinch, nz=Math.max(2, Math.min(19, P.z+Math.log2(p.d/P.d)));
    MAP.z=nz; MAP.lon=mlon(mpx(P.lon,nz)-p.cx+p.w/2, nz); MAP.lat=mlat(mpy(P.lat,nz)-p.cy+p.h/2, nz);
    mapDrawSoon(); return;
  }
  const d=MAP.drag; if (!d) return;
  const dx=e.clientX-d.x, dy=e.clientY-d.y;
  if (!d.moved && Math.hypot(dx, dy) < 4) return;
  d.moved=true; MAP.lon=mlon(d.px-dx, MAP.z); MAP.lat=mlat(d.py-dy, MAP.z); mapDrawSoon();
});
function mapUp(e){
  if (!mapPtrs.delete(e.pointerId)) return;
  if (MAP.pinch){
    if (mapPtrs.size>=2) return;
    MAP.pinch=null;
    // the finger still down carries on dragging from where it is
    const p=[...mapPtrs.values()][0];
    MAP.drag = p ? {x:p.x, y:p.y, px:mpx(MAP.lon,MAP.z), py:mpy(MAP.lat,MAP.z), moved:true} : null;
    if (!p) $('mapview').classList.remove('drag');
    return;
  }
  const d=MAP.drag; if (!d || mapPtrs.size) return;
  MAP.drag=null; $('mapview').classList.remove('drag');
  if (!d.moved && d.id){ MAP.sel = MAP.sel===d.id ? null : d.id; mapDraw(); }
}
$('mapview').addEventListener('pointerup', mapUp);
$('mapview').addEventListener('pointercancel', mapUp);
$('mapside').addEventListener('click', e=>{
  const it=e.target.closest('.it'); if (!it) return;
  const r=radios.get(it.dataset.id); if (!r || r.lat==null) return;
  MAP.sel=r.id; MAP.lat=r.lat; MAP.lon=r.lon; MAP.focus=null; mapDraw();
});
window.addEventListener('resize', ()=>{ if (MAP.open) mapDraw(); });

// running call timers and "heard ... ago"
setInterval(()=>{
  const now=Date.now()/1000;
  for (const el of document.querySelectorAll('.act.live .dur')) el.textContent=fmtClock(now-(+el.dataset.t));
  for (const [id, c] of onAir) if (!c.live || (acts.get(c.id) && !acts.get(c.id).live)) onAir.delete(id);
  if (MAP.open) mapSide(now);
}, 1000);
setInterval(()=>{ if (radios.size) renderRadios(); }, 5000);

//---------------------------------------------------------------- system picture
function renderSystem(m){
  const kv=rows=>'<div class="kv">'+rows.filter(r=>r[1]!==undefined && r[1]!=='' && r[1]!==null)
    .map(([k,v])=>'<span class="k">'+escH(k)+'</span><span class="v">'+escH(v)+'</span>').join('')+'</div>';
  const mhz=f=>(f/1e6).toFixed(5)+' MHz';
  let h='';
  if (m.dmr){ const d=m.dmr;
    h+='<div><h4>DMR '+(d.ms?'direct mode':'repeater')+'</h4>'+kv([['Color code',d.cc],['System code',d.syscode],['Vendor',[d.brand,d.sub].filter(x=>x).join(' ')],['Site',d.site],['Rest channel',d.rest]])+'</div>'; }
  if (m.p25){ const p=m.p25;
    h+='<div><h4>P25 site</h4>'+kv([['WACN',p.wacn],['System',p.sys],['NAC',p.nac],['RFSS / site',p.rfss!=null?p.rfss+' / '+(p.site!=null?p.site:'?'):undefined],
      ['Control channel',p.cc?mhz(p.cc)+(p.cc_ch?' (ch '+p.cc_ch+')':''):undefined],['Services',p.ssc?'0x'+p.ssc:undefined],['Network time',p.time]])+'</div>';
    if (p.sec && p.sec.length) h+='<div><h4>Secondary control channels</h4>'+kv(p.sec.map(s=>['ch '+s.ch, s.f?mhz(s.f):'frequency not known yet']))+'</div>';
    if (p.nb && p.nb.length) h+='<div><h4>Neighbor sites</h4>'+kv(p.nb.map(n=>['RFSS '+n.rfss+' site '+n.site,
      (n.f?mhz(n.f):'ch '+n.ch)+(n.sys?', SYS '+n.sys:'')+(n.fail?', failed':'')+(n.conv?', conventional':'')]))+'</div>';
    if (p.iden && p.iden.length) h+='<div><h4>Channel plan</h4>'+kv(p.iden.map(i=>['Identifier '+i.id, mhz(i.base)+' + n × '+i.step+' kHz'+(i.tdma?' (TDMA)':'')]))+'</div>';
  }
  if (m.nxdn){ const n=m.nxdn; h+='<div><h4>NXDN</h4>'+kv([['RAN',n.ran],['System code',n.sys],['Site code',n.site]])+'</div>'; }
  setHTML($('sysinfo'), h ? '<div class="sysg">'+h+'</div>' : '<div class="hint">System details show up here once a repeater or control channel identifies itself.</div>');
  sysPic = m; renderTrunkNow(); renderTalkgroups();
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
function bmMsg(t, bad){ const e=$('bm-msg'); e.textContent=t||''; e.style.color=bad?'var(--fail)':''; clearTimeout(bmMsgTimer); if (t && !bad) bmMsgTimer=setTimeout(()=>{ e.textContent=''; }, 4000); }

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
    h.textContent = bms.length ? 'No bookmark matches.' : 'No bookmarks yet. Tune a channel and press the star or Add.';
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
    row.tabIndex=0;
    const n=document.createElement('span'); n.className='bm-n'; n.textContent=b.name;
    const f=document.createElement('span'); f.className='bm-f'; f.textContent=fmtMHz(b.freq);
    row.append(n, f);
    if (b.mode){ const t=document.createElement('span'); t.className='tag'; t.textContent=MODE_LABEL[b.mode]||b.mode; row.append(t); }
    const e=document.createElement('button'); e.innerHTML=icon('edit'); e.title='Edit'; e.setAttribute('aria-label', 'Edit '+b.name);
    e.onclick=(ev)=>{ ev.stopPropagation(); bmFormOpen(b); };
    const x=document.createElement('button'); x.innerHTML=icon('x'); x.title='Delete'; x.setAttribute('aria-label', 'Delete '+b.name);
    x.onclick=(ev)=>{ ev.stopPropagation(); if (confirm('Delete bookmark "'+b.name+'"?')) send({cmd:'bm_delete', id:b.id}); };
    row.append(e, x);
    row.onclick=()=>bmTune(b);
    row.onkeydown=(ev)=>{ if (ev.target===row && (ev.key==='Enter' || ev.key===' ')){ ev.preventDefault(); bmTune(b); } };
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
  $('d-bm').innerHTML = cur ? icon('star-on')+' '+escH(cur.name) : '';
  $('btn-bm').innerHTML = icon(cur ? 'star-on' : 'star');
  $('btn-bm').title = cur ? 'Edit bookmark "'+cur.name+'"' : 'Bookmark this frequency';
  $('btn-bm').setAttribute('aria-label', $('btn-bm').title);
}

function bmFormOpen(b){
  if ($('card-bm').classList.contains('folded')) foldSet('card-bm', false);
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
  if (!r){ $('bs-stat').textContent='Enter the range to sweep, from and to in MHz.'; $('bs-stat').style.color='var(--fail)'; return; }
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
for (const id of ['bs-skip','bs-keep']) $(id).onclick=()=>{ reflect(id, !$(id).classList.contains('on')); send(Object.assign({cmd:'bscan_opts'}, bsOpts())); bsSave(); };
for (const id of ['bs-thr','bs-excl']) $(id).onchange=()=>{ send(Object.assign({cmd:'bscan_opts'}, bsOpts())); bsSave(); };

function renderBscan(bs, d){
  if (!bs) return;
  const on = bs.state !== 'off', mhz = f => fmtMHz(f)+' MHz';
  setTxt($('bs-badge'), {off:'Off', sweep:'Sweeping', probe:'Probing', found:'Locked', fixed:'Found'}[bs.state] || bs.state);
  $('bs-badge').className = 'badge '+(bs.state==='found' || bs.state==='fixed' ? 'live' : (on ? 'warn' : ''));
  setTxt($('st-bs'), bs.state==='fixed' ? 'Band scan found' : 'Band scan');
  $('st-bs').className = 'badge '+(bs.state==='fixed' ? 'live' : 'warn')+(on ? '' : ' vh');
  $('bs-start').disabled = on; $('bs-stop').disabled = !on; $('bs-cont').disabled = bs.state !== 'fixed';
  bsCur = bs.state === 'fixed' ? bs.last_freq : (bs.state === 'probe' || bs.state === 'found' ? bs.freq : 0);
  bsOff = !on;
  $('bs-prog').style.width = on && bs.wins ? Math.min(100, 100*bs.win/bs.wins)+'%' : '0%';
  if (on){ // another browser may have started it: show its settings
    reflect('bs-skip', bs.skip_known); reflect('bs-keep', bs.keep_going);
    if (document.activeElement !== $('bs-thr')) $('bs-thr').value = bs.threshold;
  }
  const tail = '. Pass '+bs.pass+', window '+bs.win+' of '+bs.wins+(bs.found ? ', '+bs.found+' found' : '')+(bs.skipping ? ', '+bs.skipping+' non-digital skipped' : '')+(bs.excluded ? ', '+bs.excluded+' excluded' : '')+'.';
  let t, short = '';
  if (bs.state === 'sweep'){ t = 'Sweeping '+mhz(bs.freq)+tail; short = 'Band scan: sweeping '+mhz(bs.freq); }
  else if (bs.state === 'probe'){
    t = 'Signal at '+mhz(bs.freq)+': trying '+esc(d.name)+' ('+bs.cand+' of '+bs.ncand+')'+(bs.left ? ', '+bs.left+' more in this window' : '')+tail;
    short = 'Band scan: trying '+esc(d.name)+' on '+mhz(bs.freq);
  }
  else if (bs.state === 'found'){ t = 'Decoding '+(bs.desc || esc(d.name))+' on '+mhz(bs.freq)+', collecting ids for the bookmark…'; short = 'Band scan: locked on '+mhz(bs.freq); }
  else if (bs.state === 'fixed'){ t = 'Found '+bs.last_name+' on '+mhz(bs.last_freq)+' and bookmarked it (group "Band scan"). Continue keeps looking.'; short = 'Band scan: found '+bs.last_name; }
  else if (bs.msg) t = 'Band scan '+bs.msg;
  else t = 'Sweeps the bands with the RTL-SDR, tries every signal it sees with the digital decoders, and stops on the first one that decodes, saving it as a bookmark.'
    + (bs.found ? ' Last find: '+bs.last_name+' on '+mhz(bs.last_freq)+'.' : '');
  $('bs-stat').textContent = t;
  $('bs-stat').style.color = !on && bs.msg ? 'var(--fail)' : '';
  if (short) $('ad-stat').textContent = short;
}

//---------------------------------------------------------------- files
function renderFiles(m){
  const el=$('filelist'); el.innerHTML='';
  if (!m.files || !m.files.length){ el.innerHTML='<div class="hint">No recordings yet. Turn on Per-call WAV to record each call.</div>'; return; }
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
function bindToggle(id, cmd, key){ $(id).onclick=()=>{ const on=!$(id).classList.contains('on'); const o={cmd}; o[key]=on; send(o); reflect(id, on); }; }

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
$('tg-inv').onclick=()=>{ const on=!$('tg-inv').classList.contains('on'); send({cmd:'inversion', inverted:on}); reflect('tg-inv', on); };
$('tg-s1').onclick=()=>{ const on=!$('tg-s1').classList.contains('on'); send({cmd:'slots', slot1:on}); reflect('tg-s1', on); };
$('tg-s2').onclick=()=>{ const on=!$('tg-s2').classList.contains('on'); send({cmd:'slots', slot2:on}); reflect('tg-s2', on); };
$('gdig').oninput=()=>{ const v=+$('gdig').value; $('gdigv').textContent=v?v:'auto'; send({cmd:'gain', digital:v}); };
$('gana').oninput=()=>{ const v=+$('gana').value; $('ganav').textContent=v; send({cmd:'gain', analog:v}); };
bindToggle('tg-hpfd','filters','hpfd'); bindToggle('tg-hpf','filters','hpf');
bindToggle('tg-lpf','filters','lpf'); bindToggle('tg-pbf','filters','pbf');
bindToggle('tg-payload','payload','enable'); bindToggle('tg-monitor','monitor','enable');
function toggleWav(id){ const on=!$(id).classList.contains('on'); send({cmd:'per_call_wav', enable:on}); reflect('tg-wav',on); reflect('tg-wav2',on); }
$('tg-wav').onclick=()=>toggleWav('tg-wav'); $('tg-wav2').onclick=()=>toggleWav('tg-wav2');
$('btn-resetev').onclick=clearAll;
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
// squelch: Auto floor follows the spectrum's noise floor; Set and Channel floor fix a level and turn it off
$('tg-sqauto').onclick=()=>{ const on=!$('tg-sqauto').classList.contains('on'); reflect('tg-sqauto', on); send({cmd:'rtl', sql_auto:on}); };
$('sqmargin').onchange=()=>send({cmd:'rtl', sql_margin:+$('sqmargin').value});
// the arrows (spin buttons, arrow keys) apply the level as it steps; a typed number waits for Set or Enter
let sqTimer=0;
const sqApply=()=>{ clearTimeout(sqTimer); const v=Math.round(+$('rtlsq').value);
  if ($('rtlsq').value==='' || !(v>=0)) return; reflect('tg-sqauto', false); send({cmd:'rtl', squelch:v}); };
$('rtlsq').addEventListener('input', e=>{ if (!e.inputType){ clearTimeout(sqTimer); sqTimer=setTimeout(sqApply, 250); } });
$('rtlsq').addEventListener('keydown', e=>{ if (e.key==='Enter') sqApply(); });
$('btn-sq').onclick=sqApply;
$('btn-sqchan').onclick=()=>{ reflect('tg-sqauto', false); send({cmd:'rtl', sql_chan:true}); };
$('btn-smb').onclick=()=>send({cmd:'setmod_bw', hz:+$('setmodbw').value});

// Settings → Trunk: lists and the conventional scanner (trunking itself is in its card)
$('tg-scan').onclick=()=>{ const on=!$('tg-scan').classList.contains('on'); send({cmd:'scanner', enable:on}); };
$('btn-cycle').onclick=()=>send({cmd:'cycle'});
$('btn-chan').onclick=()=>send({cmd:'csv', kind:'channel', text:$('csv-chan').value});
$('btn-group').onclick=()=>send({cmd:'csv', kind:'group', text:$('csv-group').value});
// the names and choices as they are, in the group CSV format the import (and -G) reads
$('btn-group-cur').onclick=()=>{
  const rows=['DEC,Mode,Name,Tag'];
  for (const [id,g] of [...groups].sort((a,b)=>a[0]-b[0])) rows.push(id+','+(g.m || 'A')+','+(g.n||'').replace(/,/g,' '));
  $('csv-group').value=rows.join('\n');
};
$('btn-scanset').onclick=()=>{ const fs=$('scanlist').value.split(/\s+/).map(parseFreq).filter(x=>x); send({cmd:'scan_set', freqs:fs}); };
$('btn-scanbm').onclick=()=>{ $('scanlist').value=bmFiltered().slice(0,26).map(b=>(b.freq/1e6).toFixed(6)+'M').join('\n'); };

// auto
$('tg-autotune').onclick=()=>{ const on=!$('tg-autotune').classList.contains('on'); send({cmd:'auto_tune', enable:on}); reflect('tg-autotune', on); };
$('tg-autodec').onclick=()=>{ const on=!$('tg-autodec').classList.contains('on'); send({cmd:'auto_decode', enable:on}); reflect('tg-autodec', on); };
$('tg-autoana').onclick=()=>{ const on=!$('tg-autoana').classList.contains('on'); send({cmd:'auto_analog', enable:on}); reflect('tg-autoana', on); };

// keys
const khints={bp:'Decimal key id (0-255)',scrambler:'Decimal (1-32767)',rc4:'Hex, e.g. 0102030405',des:'Hex 16 chars',aes:'Hex, space every 16 (128/256-bit)',hytera:'Hex 10/32/64 chars',tyt_bp:'Hex 4 chars',tyt_ap:'Hex 32/64 chars',tyt_ep:'Hex 32 chars',retevis:'Hex 32/64 chars',baofeng:'Hex 32/64 chars',anytone:'Hex 4 chars',kenwood:'Decimal (15-bit)',csi:'Hex 18 chars (72-bit)',vertex:'10:xxxxxxxxxx or 64:...',auctus:'8 chars',straight:'len:hexbytes[:discard]',ras:'password (6-24 chars)'};
$('ktype').onchange=()=>{ $('khint').textContent=khints[$('ktype').value]||''; };
$('ktype').onchange();
$('btn-key').onclick=()=>send({cmd:'key', keytype:$('ktype').value, value:$('kval').value});
$('btn-keyclear').onclick=()=>send({cmd:'key', keytype:'clear'});
$('tg-autokey').onclick=()=>{ const on=!$('tg-autokey').classList.contains('on'); send({cmd:'auto_key', enable:on}); reflect('tg-autokey', on); };
$('tg-force').onclick=()=>{ const on=!$('tg-force').classList.contains('on'); send({cmd:'key', keytype: on?'force':'unforce'}); reflect('tg-force', on); };
$('btn-keycsv').onclick=()=>send({cmd:'csv', kind:$('kcsvkind').value, text:$('csv-keys').value});

// files
$('btn-refresh').onclick=()=>send({cmd:'list_files', area:'wav'});

// spectrum config
function pushSpec(){ send({cmd:'spectrum', fft:+$('fftsize').value, fps:+$('fps').value, avg:+$('avg').value}); }
$('fftsize').onchange=pushSpec; $('fps').onchange=pushSpec; $('avg').onchange=pushSpec;

// tabs
$('tabs').addEventListener('click', e=>{ if(e.target.tagName!=='BUTTON') return;
  const t=e.target.dataset.t;
  for (const b of $('tabs').children){ b.classList.toggle('on', b.dataset.t===t); b.setAttribute('aria-selected', b.dataset.t===t); }
  for (const p of document.querySelectorAll('.tabpane')) p.classList.toggle('on', p.dataset.t===t);
  if (t==='files') send({cmd:'list_files', area:'wav'});
});

// login (a form, so Enter submits and password managers know the field)
$('pw-form').onsubmit=(e)=>{ e.preventDefault(); send({cmd:'auth', password:$('pw').value}); };

//---------------------------------------------------------------- folding cards
// Bookmarks and Band scan fold down to their title bar; remembered per browser
function foldSet(id, folded){
  const card=$(id), b=card.querySelector('button.fold'), name=card.querySelector('h2').firstChild.textContent.trim().toLowerCase();
  card.classList.toggle('folded', folded); // the chevron turns with the class
  b.setAttribute('aria-expanded', String(!folded));
  b.title = (folded ? 'Show ' : 'Hide ')+name;
  try { const f=JSON.parse(localStorage.getItem('dsdfme.fold') || '{}'); f[id]=folded; localStorage.setItem('dsdfme.fold', JSON.stringify(f)); } catch(e){}
}
for (const b of document.querySelectorAll('button.fold')) b.onclick=()=>foldSet(b.dataset.fold, !$(b.dataset.fold).classList.contains('folded'));
(function(){ try { const f=JSON.parse(localStorage.getItem('dsdfme.fold') || '{}'); for (const id in f) if (f[id] && $(id)) foldSet(id, true); } catch(e){} })();

for (const b of document.querySelectorAll('button.tgl')) b.setAttribute('aria-pressed', b.classList.contains('on'));
slotInit();
drawRadios();
sizeCanvas();
renderBookmarks();
connect();
