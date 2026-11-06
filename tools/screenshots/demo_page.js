// Runs inside the page (shoot.mjs sends it with the DevTools protocol): replaces what the decoder reports
// with made-up demo data. The radios, names, talkgroups, positions and messages are invented. The
// spectrum and waterfall stay live, from the simulated receiver (fakertl.c).
(() => {
  const now = Math.floor(Date.now() / 1000);
  const R = {
    3101: { n: 'Airboat 3', a: 'AIRBOAT 3', lat: 25.76128, lon: -80.48207, v: 41 },
    3104: { n: 'Gator Patrol', a: 'GATOR PTL', lat: 25.74512, lon: -80.68044, v: 18 },
    3107: { n: 'Truck 7', a: 'TRUCK 7', lat: 25.77431, lon: -80.19372, v: 32 },
    3112: { n: 'Van 12', a: 'VAN 12', lat: 25.79065, lon: -80.13002, v: 0 },
    3120: { n: 'Dispatch', a: 'DISPATCH' },
    3125: { n: 'Swamp Ops', a: 'SWAMP OPS', lat: 25.85788, lon: -80.34101, v: 0 },
  };
  const TG = { 1: 'Ops', 2: 'Maintenance', 9: 'Local' };
  const items = [];
  let id = 100;
  const pos = (r) => r.lat.toFixed(5) + ', ' + r.lon.toFixed(5);
  const call = (dt, s, src, tg, dur, live) => {
    const r = R[src], it = { id: ++id, k: 'call', p: 'DMR', s, t: now - dt, ty: 'Group call', src, sn: r.n, dst: tg, dn: TG[tg], g: 0,
      live: live ? 1 : 0, dur, f: [['Talker alias', r.a]], sum: src + ' → TG ' + tg + ' (' + r.a + ')' };
    if (live && r.lat != null) { it.lat = r.lat; it.lon = r.lon; it.f.push(['GPS', pos(r)]); }
    items.push(it);
  };
  const lrrp = (dt, s, src) => {
    const r = R[src];
    items.push({ id: ++id, k: 'data', p: 'DMR', s, t: now - dt, ty: 'LRRP', src, sn: r.n, lat: r.lat, lon: r.lon,
      f: [['Position', pos(r)], ['Speed', r.v + ' km/h']], sum: 'Position ' + pos(r) + ', ' + r.v + ' km/h' });
  };
  const ars = (dt, s, src) => items.push({ id: ++id, k: 'data', p: 'DMR', s, t: now - dt, ty: 'ARS', src, sn: R[src].n, sum: 'Registration' });

  ars(610, 1, 3125); lrrp(560, 1, 3125);
  call(500, 2, 3120, 9, 6);
  ars(455, 1, 3104); lrrp(430, 1, 3104);
  call(380, 2, 3112, 2, 11); lrrp(330, 2, 3112);
  items.push({ id: ++id, k: 'data', p: 'DMR', s: 1, t: now - 300, ty: 'Text', src: 3107, sn: 'Truck 7', dst: 3120, dn: 'Dispatch', g: 1,
    sum: 'Gator on the boat ramp again. Not touching it.' });
  items.push({ id: ++id, k: 'sig', p: 'DMR', s: 1, t: now - 296, ty: 'Text ack', src: 3120, sn: 'Dispatch', dst: 3107, dn: 'Truck 7', g: 1,
    ack: 'delivered', sum: 'Delivered' });
  call(240, 1, 3104, 1, 17);
  lrrp(180, 2, 3107);
  call(120, 1, 3120, 1, 8);
  lrrp(75, 1, 3101);
  call(14, 2, 3107, 2, 0, true);
  call(7, 1, 3101, 1, 0, true);

  const slot = (src, tg, gps, data) => ({ tg, src, gi: 0, burst: 'VOICE', err: '00', err2: '', on: 1, alias: R[src].a, call: '', gps, data });
  const demoStatus = {
    decode: { carrier: 1, analog: 0, name: 'DMR', sys: 'DMR', ftype: 'Voice', level: 64, stereo: 1 },
    slot1: slot(3101, 1, pos(R[3101]), 'LRRP position, 41 km/h'),
    slot2: slot(3107, 2, '', ''),
  };
  const demoSystem = { type: 'system', dmr: { ms: 0, cc: 1 } };

  const real = window.__realHandleJSON || handleJSON;
  window.__realHandleJSON = real;
  handleJSON = function (m) {
    if (m.type === 'status') { m.decode = Object.assign(m.decode || {}, demoStatus.decode); m.slot1 = demoStatus.slot1; m.slot2 = demoStatus.slot2; }
    if (m.type === 'system') m = demoSystem;
    if (m.type === 'activity' && !m.demo) return; // nothing the decoder logs reaches the page
    return real(m);
  };
  handleJSON({ type: 'activity', demo: 1, reset: true, items });
  handleJSON(demoSystem);
  return items.length + ' demo activity items';
})()
