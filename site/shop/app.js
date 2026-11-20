// The dirtyloader mod shop: the catalog's mods as cards, installed straight onto a connected M8 (m8.js), and the M8's
// theme, edited here: the page wears it as you pick it, and the M8 does too.
import { M8, UnitError, parseM8x, sha256hex, describeFw, webSerialTransport, TEENSY_VID } from './m8.js';
import { initGuide, renderStart, guideSawDevice } from './guide.js';
import { SLOTS, PRESETS, EXT_SLOTS, EXT_PRESETS, parseM8t, encodeM8t, applyTokens, drawScreen, toHex, setHex } from './theme.js';

const $ = (s, el = document) => el.querySelector(s);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private window */ } },
};
const TONE = { peak: 'var(--peak)', scope: 'var(--scope)', cursor: 'var(--cursor)', mid: 'var(--mid)', info: 'var(--info)', title: 'var(--title)' };
const hexOf = (c) => [...c].map(b => b.toString(16).padStart(2, '0')).join('');
const bytesOf = (h) => Uint8Array.from(h.match(/../g).map(x => parseInt(x, 16)));

const state = {
  catalog: null,
  cat: 'all', target: store.get('dl.target', 'all'), sort: 'collection', query: '', showDev: store.get('dl.dev', false),
  m8: null, info: null, mods: [], status: '', busy: new Map(),   // busy: mod id -> progress 0..1
  theme: null, themeEdited: false, deviceTheme: null, deviceThemes: [], themeOk: false, liveDirty: false,
  mine: store.get('dl.mythemes', []),
  ext: null, extEdited: false, deviceExt: null, extOk: false,
};

// ---- toasts ------------------------------------------------------------------------------------------------------
function toast(msg, { kind = '', action, ms = 6000 } = {}) {
  const t = document.createElement('div');
  t.className = 'toast ' + kind;
  t.innerHTML = `<span>${esc(msg)}</span>`;
  if (action) {
    const b = document.createElement('button');
    b.className = 'btn primary small'; b.textContent = action.label;
    b.onclick = () => { t.remove(); action.run(); };
    t.append(b);
  }
  $('#toasts').append(t);
  setTimeout(() => t.remove(), ms);
}

// ---- cover art: line drawings per motif, in the category's tone, varied by the mod's id ----------------------------
function rng(seed) { let s = 0; for (const ch of seed) s = (s * 31 + ch.charCodeAt(0)) >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }
function motif(kind, id) {
  const r = rng(id), L = [];
  const path = (d, w = 2.2, o = 0.9) => L.push(`<path d="${d}" fill="none" stroke="var(--value)" stroke-width="${w}" stroke-linecap="round" stroke-linejoin="round" opacity="${o}"/>`);
  const box = (x, y, w, h, o = 0.85) => L.push(`<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="1.5" fill="var(--value)" opacity="${o}"/>`);
  const wave = (f, a, y0, ph = 0, x0 = 0, x1 = 300, damp = 0) => { let d = ''; for (let x = x0; x <= x1; x += 3) { const k = damp ? Math.exp(-(x - x0) * damp) : 1; d += (x === x0 ? 'M' : 'L') + x + ' ' + (y0 + Math.sin(x * f + ph) * a * k).toFixed(1); } return d; };
  switch (kind) {
    case 'pad': {           // FXPAD: fine LED cells in a two-armed swirl round a finger (the mod's own maths)
      const cx = 150 + (r() - 0.5) * 50, cy = 64, tw = 0.9;
      for (let j = 0; j < 16; j++) for (let i = 0; i < 52; i++) {
        const x = 8 + i * 5.6, y = 22 + j * 5.6, dx = (x - cx) / 19, dy = (y - cy) / 19;
        let f = 2 * (Math.atan2(dy, dx) / (2 * Math.PI)) + tw * 0.5 * Math.log2(dx * dx + dy * dy + 0.05);
        f -= Math.floor(f);
        const band = Math.floor(f * 8);
        if (band !== 0 && band !== 4) box(x, y, 4.6, 4.6, 0.2 + 0.11 * band);
      }
      path(`M${cx - 10} ${cy}h20M${cx} ${cy - 10}v20`, 3.2, 1); break;
    }
    case 'lfo': {
      path(wave(0.045, 22, 60, r() * 6, 14, 190), 2.6);
      for (let k = 0; k < 4; k++) { const y = 30 + k * 18; path(`M200 60 L232 ${y + 4}`, 1.5, 0.6); L.push(`<rect x="236" y="${y - 3}" width="46" height="14" rx="3" fill="none" stroke="var(--value)" stroke-width="1.8" opacity=".85"/>`); }
      break;
    }
    case 'reverb': for (let k = 0; k < 22; k++) { const x = 24 + k * 12, h = 70 * Math.exp(-k * 0.13) * (0.6 + r() * 0.4); path(`M${x} ${100} V${100 - h}`, 3, 0.85); } path('M14 100 H290', 1.5, .5); break;
    case 'meter': for (let k = 0; k < 14; k++) { const h = 20 + r() * 62; box(22 + k * 19, 100 - h, 12, h, 0.35 + 0.5 * (h / 82)); } break;
    case 'load': for (let k = 0; k < 6; k++) { const y = 92 - k * 12; path(`M${30 + k * 6} ${y} H${120 + r() * 150}`, 6, 0.3 + k * 0.1); } break;
    case 'api': path('M60 30 Q40 30 40 50 Q40 60 28 62 Q40 64 40 74 Q40 94 60 94', 3); path('M240 30 Q260 30 260 50 Q260 60 272 62 Q260 64 260 74 Q260 94 240 94', 3); for (let k = 0; k < 5; k++) box(80 + k * 30 + r() * 6, 58, 12, 8); break;
    case 'test': for (let k = 0; k < 4; k++) { const y = 34 + k * 18; path(`M30 ${y} l6 6 l12 -12`, 3); path(`M62 ${y} H${150 + r() * 120}`, 3, .45); } break;
    case 'flash': L.push('<rect x="105" y="30" width="90" height="64" rx="6" fill="none" stroke="var(--value)" stroke-width="2.6" opacity=".9"/>'); for (let k = 0; k < 7; k++) { path(`M${113 + k * 12} 30 V18`, 2, .7); path(`M${113 + k * 12} 94 V106`, 2, .7); } path('M140 50 l14 0 l-8 12 h12', 3); break;
    case 'pulse': path(`M10 66 H${90 + r() * 20} l10 -40 l14 70 l10 -46 l8 16 H290`, 2.8); break;
    case 'tone': path(wave(0.06, 30, 64, r() * 6, 10, 290), 2.8); break;
    case 'flat': path('M10 64 H130 l8 -10 l8 20 l8 -10 H290', 2.6); break;
    default: path(wave(0.05, 20, 64), 2.4);
  }
  return L.join('');
}
function coverSVG(mod, cats) {
  const tone = TONE[cats[mod.category]?.tone] || 'var(--cursor)';
  const gid = 'g' + mod.id.replace(/[^a-z0-9]/gi, '');
  return `<svg viewBox="0 0 300 112" preserveAspectRatio="xMidYMid slice" aria-hidden="true">
    <defs><linearGradient id="${gid}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" style="stop-color:${tone}"/><stop offset="1" style="stop-color:${tone};stop-opacity:.35"/></linearGradient></defs>
    <rect width="300" height="112" fill="var(--bg)"/><rect width="300" height="112" fill="url(#${gid})" opacity=".85"/>${motif(mod.motif, mod.id)}</svg>`;
}

// ---- the catalog ---------------------------------------------------------------------------------------------------
const targetsOf = () => state.catalog.targets;
const deviceTarget = () => state.info?.target || null;
function buildFor(mod) {
  const t = deviceTarget() || (state.target !== 'all' ? state.target : null);
  return t ? mod.builds[t] : Object.values(mod.builds)[0];
}
function installed(mod) { return state.mods.find(m => m.head?.id === mod.id) || null; }

function visibleMods() {
  const q = state.query.trim().toLowerCase();
  let list = state.catalog.mods.filter(m =>
    (state.showDev || !m.dev || installed(m)) &&
    (state.cat === 'all' || m.category === state.cat) &&
    (state.target === 'all' || m.builds[state.target]) &&
    (!q || [m.id, m.title, m.summary, m.category, ...(m.hooks || [])].join(' ').toLowerCase().includes(q)));
  if (state.sort === 'name') list = [...list].sort((a, b) => a.title.localeCompare(b.title));
  if (state.sort === 'size') list = [...list].sort((a, b) => (buildFor(a)?.size || 0) - (buildFor(b)?.size || 0));
  return list;
}

function renderSide() {
  const cats = state.catalog.categories;
  const base = state.catalog.mods.filter(m => state.showDev || !m.dev);
  const items = [['all', 'All mods', 'linear-gradient(135deg, var(--peak), var(--cursor))', base.length],
    ...Object.entries(cats).map(([k, c]) => [k, c.label, TONE[c.tone], base.filter(m => m.category === k).length])].filter(i => i[3] || i[0] === 'all');
  $('#nav-cats').innerHTML = items.map(([k, label, color, n]) =>
    `<li><button type="button" data-cat="${k}" class="${state.cat === k ? 'on' : ''}"><span class="dot" style="background:${color}"></span>${esc(label)}<span class="n">${n}</span></button></li>`).join('');
  $('#nav-count').textContent = state.mods.length;
}

function renderChips() {
  const t = targetsOf();
  const shown = state.catalog.mods.filter(m => state.showDev || !m.dev || installed(m));
  const shared = (v) => Object.values(t).filter(x => x.label === v.label).length > 1;   // Model:02 6.6.2A and 6.6.4A
  const opts = [['all', 'All', shown.length], ...Object.entries(t).map(([k, v]) => [k, shared(v) ? `${v.label} ${v.firmware}` : v.label, shown.filter(m => m.builds[k]).length])];
  $('#target-chips').innerHTML = opts.map(([k, label, n]) => `<button type="button" class="chip ${state.target === k ? 'on' : ''}" data-target="${k}">${esc(label)}<span class="n">${n}</span></button>`).join('');
  const dt = deviceTarget();
  $('#target-note').textContent = state.target === 'm8-model01'
    ? 'Download a Model:01 build and copy it to dirtyloader/mods on the SD card.'
    : dt ? `Your M8 is a ${state.info.label} on ${state.info.version}: Install puts that build on it.`
    : state.target === 'all' ? "Every model's mods. Connect your M8, or pick its model, to see the ones that fit it." : `Mods built for the ${t[state.target].label} (firmware ${t[state.target].firmware}).`;
}

function pillFor(mod) {
  const busy = state.busy.get(mod.id);
  if (busy !== undefined) return `<button class="pill" disabled>${busy < 1 ? Math.round(busy * 100) + '%' : 'Checking'}</button>`;
  const dt = deviceTarget();
  const selected = dt || state.target;
  if (mod.builds['m8-model01'] && (selected === 'm8-model01' || (selected === 'all' && Object.keys(mod.builds).length === 1))) {
    return `<a class="pill" href="${esc(mod.builds['m8-model01'].file)}" download>Download</a>`;
  }
  if (dt && !mod.builds[dt]) return `<button class="pill" disabled>Not for ${esc(state.info.label.replace('M8 ', ''))}</button>`;
  const inst = installed(mod);
  if (inst) {
    const want = buildFor(mod)?.version;
    if (want && inst.head.version !== want) return `<button class="pill upd" data-install="${mod.id}">Update to ${esc(want)}</button>`;
    return `<button class="pill in" data-goto="${esc(inst.name)}">&#10003; ${inst.on ? 'Installed' : 'Installed, off'}</button>`;
  }
  return `<button class="pill" data-install="${mod.id}">+ Install</button>`;
}

function renderShelves() {
  const cats = state.catalog.categories;
  const mods = visibleMods();
  const groups = state.sort === 'collection' ? Object.keys(cats).map(k => [k, mods.filter(m => m.category === k)]).filter(g => g[1].length) : [['', mods]];
  if (!mods.length) { $('#shelves').innerHTML = '<p class="empty">No mods match.</p>'; return; }
  $('#shelves').innerHTML = groups.map(([k, list]) => `<section class="shelf">${k ? `<h3>${esc(cats[k].label)}<span class="n">${list.length}</span></h3>` : ''}
    <div class="cards">${list.map(card).join('')}</div></section>`).join('');
}
function card(mod) {
  const b = buildFor(mod), t = targetsOf();
  const models = Object.keys(mod.builds).map(k => t[k].label.replace('M8 ', '') + ' ' + t[k].firmware).join(' · ');
  const inst = installed(mod);
  const prog = state.busy.get(mod.id);
  return `<article class="card ${inst ? 'installed' : ''}" data-id="${mod.id}">
    <button class="cover" data-details="${mod.id}" aria-label="Details: ${esc(mod.title)}">${coverSVG(mod, state.catalog.categories)}<span class="cv-title">${esc(mod.id.replace(/-/g, ' '))}</span></button>
    ${prog !== undefined ? `<div class="prog"><i style="width:${Math.round(prog * 100)}%"></i></div>` : ''}
    <div class="card-body">
      <div class="card-top"><h4>${esc(mod.title)}</h4><span class="ver">v${esc(b?.version || mod.version)}</span>${pillFor(mod)}</div>
      <div class="meta">${esc(models)}</div>
      <p class="summary">${esc(mod.summary)}</p>
    </div>
    <div class="card-foot"><span>${b ? (b.size / 1024).toFixed(1) + ' KB' : ''} · ${mod.hooks.length} hooks</span><span class="grow"></span>
      <button type="button" data-details="${mod.id}">Details</button><a href="${esc(mod.source)}" target="_blank" rel="noreferrer">Source</a></div>
  </article>`;
}
function rerenderCard(id) {
  const el = document.querySelector(`.card[data-id="${CSS.escape(id)}"]`);
  const mod = state.catalog.mods.find(m => m.id === id);
  if (el && mod) el.outerHTML = card(mod);
}

function details(id) {
  const m = state.catalog.mods.find(x => x.id === id);
  const t = targetsOf(), cats = state.catalog.categories, inst = installed(m);
  $('#details-body').innerHTML = `<p class="eyebrow">${esc(cats[m.category].label)}${m.dev ? ' · developer' : ''}</p>
    <h2>${esc(m.title)} <span class="ver">v${esc(m.version)}</span></h2>
    <p>${esc(m.summary)}</p>${m.details ? `<p>${esc(m.details)}</p>` : ''}
    ${m.keys?.length ? `<h3>On the M8</h3><ul>${m.keys.map(k => `<li>${esc(k)}</li>`).join('')}</ul>` : ''}
    <h3>Files</h3><dl>${Object.entries(m.builds).map(([k, b]) => `<dt>${esc(t[k].label)}</dt><dd>${esc(t[k].firmware)} · ${b.size} bytes<br><span class="hash">sha256 ${b.sha256}</span> · <a href="${esc(b.file)}" download>Download</a></dd>`).join('')}</dl>
    <h3>About it</h3><dl><dt>Hooks</dt><dd>${m.hooks.map(esc).join(', ')}</dd><dt>On a unit</dt><dd>${esc(m.on_unit || '')}</dd>
      <dt>On your M8</dt><dd>${inst ? `${esc(inst.name)}, v${esc(inst.head.version)}, ${inst.on ? 'on' : 'off'}` : state.m8 ? 'not installed' : 'connect to see'}</dd>
      <dt>Source</dt><dd><a href="${esc(m.source)}" target="_blank" rel="noreferrer">${esc(m.source.replace('https://', ''))}</a></dd><dt>Licence</dt><dd>GPL-2.0-or-later</dd></dl>`;
  $('#details').showModal();
}

// ---- the M8 -----------------------------------------------------------------------------------------------------------
async function connect(port, { quiet = false } = {}) {
  if (state.m8) return;
  try {
    const m8 = port ? new M8(await webSerialTransport(port)) : await M8.choose();
    m8.onclose = () => { if (state.m8 === m8) { state.m8 = null; state.info = null; state.mods = []; renderAll(); } };
    state.m8 = m8;
    try { state.info = await m8.info(); }
    catch (e) { await m8.close(); state.m8 = null; throw e; }
    if (state.info.target && state.target !== 'all' && state.target !== state.info.target) state.target = state.info.target;
    await refreshDevice();
    await loadDeviceThemes();
    toast(`Connected: ${state.info.label} ${state.info.version}, dirtyloader core ${state.info.core}`);
    guideSawDevice(state.info);
  } catch (e) {
    if (e?.name === 'NotFoundError') return;          // the chooser was closed
    if (quiet) { renderAll(); return; }                 // reconnecting after a restart: it may not be up yet
    toast(e instanceof UnitError ? e.message : (e.message || String(e)), { kind: 'bad', ms: 9000 });
  }
  renderAll();
}
async function refreshDevice() {
  if (!state.m8) return;
  state.mods = await state.m8.mods();
  state.status = await state.m8.status();
  try { state.info = await state.m8.info(); } catch { /* keep the last */ }
}
function pendingRestart() {
  if (!state.info) return false;
  const want = new Set(state.mods.filter(m => m.on && m.head).map(m => m.head.id));
  const run = new Set(state.info.loaded);
  return [...want].some(x => !run.has(x)) || [...run].some(x => !want.has(x));
}
function statusLine(name) {
  const l = state.status.split('\n').find(s => s.startsWith(name + ':'));
  return l ? l.slice(name.length + 1).trim() : '';
}

async function install(mod) {
  if (!state.m8) { await connect(); if (!state.m8) return; }
  const t = deviceTarget(), b = mod.builds[t];
  if (!b) { toast(`${mod.title} isn't built for the ${state.info.label}.`, { kind: 'warn' }); return; }
  state.busy.set(mod.id, 0); rerenderCard(mod.id);
  try {
    const data = new Uint8Array(await (await fetch(b.file, { cache: 'no-cache' })).arrayBuffer());
    if (await sha256hex(data) !== b.sha256) throw new Error(`${mod.title}: the file is not the one the shop lists (sha256). Not sent.`);
    const head = parseM8x(data);
    if (head.fw && head.fw !== state.info.fw) throw new Error(`${mod.title}: built for ${describeFw(head.fw).label} ${describeFw(head.fw).version}, your M8 is ${state.info.label} ${state.info.version}.`);
    const name = installed(mod)?.name || mod.id + '.m8x';
    let last = 0;
    await state.m8.install(name, data, { on: true, onProgress: (p) => { if (p - last > 0.04 || p === 1) { last = p; state.busy.set(mod.id, p); rerenderCard(mod.id); } } });
    state.busy.set(mod.id, 1); rerenderCard(mod.id);
    await refreshDevice();
    toast(`${mod.title} is on your M8. Restart it to load.`, { action: { label: 'Restart', run: restart } });
  } catch (e) {
    toast(e.message || String(e), { kind: 'bad', ms: 10000 });
  } finally {
    state.busy.delete(mod.id); renderAll();
  }
}
async function installOwn(files) {
  if (!state.m8) { await connect(); if (!state.m8) return; }
  for (const f of files) {
    try {
      const data = new Uint8Array(await f.arrayBuffer());
      const head = parseM8x(data);
      if (head.fw && head.fw !== state.info.fw) throw new Error(`${f.name}: built for ${describeFw(head.fw).label} ${describeFw(head.fw).version}, your M8 is ${state.info.label} ${state.info.version}.`);
      const name = (state.mods.find(m => m.head?.id === head.id)?.name) || f.name.replace(/[^A-Za-z0-9._-]/g, '_');
      await state.m8.install(name, data, { on: true });
      toast(`${head.id} ${head.version} is on your M8. Restart it to load.`, { action: { label: 'Restart', run: restart } });
    } catch (e) { toast(e.message || String(e), { kind: 'bad', ms: 10000 }); }
  }
  await refreshDevice(); renderAll();
}
async function restart() {
  if (!state.m8) return;
  if (state.demo) {                                   // the pretend M8 "boots" with what's switched on
    state.demo.loaded = state.mods.filter(m => m.on && m.head).map(m => m.head.id);
    await refreshDevice(); renderAll(); toast('Restarted (demo).'); return;
  }
  try { await state.m8.restart(); } catch { /* it may go before it answers */ }
  try { await state.m8.close(); } catch { /* gone */ }
  state.m8 = null; state.info = null; renderAll();
  toast('Restarting your M8...', { ms: 4000 });
  reconnectSoon(4000);
}
// the M8 comes back as a new USB device after a restart or a flash: reconnect to it without asking
function reconnectSoon(after = 6000, ms = 30000) {
  state.reconnect = { until: Date.now() + after + ms };
  const tryAgain = async () => {
    if (!state.reconnect || Date.now() > state.reconnect.until || state.m8) return;
    const ports = (await navigator.serial.getPorts()).filter(p => p.getInfo().usbVendorId === TEENSY_VID);
    for (const p of ports) { try { await connect(p, { quiet: true }); if (state.m8) { state.reconnect = null; return; } } catch { /* not yet */ } }
    setTimeout(tryAgain, 1500);
  };
  setTimeout(tryAgain, after);
}

function renderDeviceBox() {
  const i = state.info;
  $('#dev-box').innerHTML = i
    ? `<div class="dev-name"><span class="state on"></span>${esc(i.label)}</div><dl><dt>Firmware</dt><dd>${esc(i.version)}</dd><dt>Core</dt><dd>${esc(i.core)}</dd>
       <dt>SD card</dt><dd>${i.sd ? 'in' : 'none'}</dd><dt>Running</dt><dd>${i.loaded.length} mods</dd></dl>`
    : `<div class="dev-name"><span class="state"></span>No M8 connected</div><p class="muted small" style="margin:4px 0 0">Plug it in with USB and press Connect.</p>`;
  $('#connect-text').textContent = i ? 'Disconnect' : 'Connect M8';
  const p = pendingRestart();
  $('#pending').hidden = !p;
  if (p) $('#pending').innerHTML = `<span>Restart to load your changes</span><button type="button" class="btn primary small" id="pending-restart">Restart M8</button>`;
  $('#g-connect').classList.toggle('done', !!i);
  $('#g-core').classList.toggle('done', !!i);
  $('#g-connect-btn').hidden = !!i;
  if (!state.connectText) state.connectText = $('#g-connect-text').innerHTML;
  $('#g-connect-text').innerHTML = i ? `Connected: ${esc(i.label)} ${esc(i.version)} with core ${esc(i.core)}.` : state.connectText;
}

function renderDevice() {
  const i = state.info;
  $('#device-title').textContent = i ? i.label : 'Your M8';
  $('#device-sub').textContent = i ? `Firmware ${i.version}, dirtyloader core ${i.core}. ${state.mods.length} mods on the card, ${i.loaded.length} running.` : 'Connect your M8 to see its mods.';
  $('#dev-actions').hidden = !i; $('#drop').hidden = !i; $('#boot').hidden = !i;
  $('#boot-text').textContent = state.status;
  if (!i) { $('#mod-rows').innerHTML = `<li class="empty">No M8 connected. <button class="btn primary small" data-connect>Connect M8</button></li>`; return; }
  if (!state.mods.length) { $('#mod-rows').innerHTML = '<li class="empty">No mods on the card yet: install some from the Mod Shop.</li>'; return; }
  $('#mod-rows').innerHTML = state.mods.map(m => {
    const h = m.head, cat = h && state.catalog.mods.find(x => x.id === h.id);
    const run = h && i.loaded.includes(h.id);
    const err = !run && m.on ? statusLine(m.name) : '';
    const [cls, label] = !h ? ['err', 'Not a valid .m8x'] : m.on ? (run ? ['run', 'Running'] : err && !err.startsWith('loaded') ? ['err', 'Not loaded'] : ['next', 'On at restart'])
      : (run ? ['next', 'Off at restart'] : ['off', 'Off']);
    const icon = cat ? coverSVG(cat, state.catalog.categories).replace('viewBox="0 0 300 112"', 'viewBox="96 0 112 112"') : '';
    return `<li class="mod-row" data-row="${esc(m.name)}"><div class="ic">${icon}</div>
      <div><h4>${esc(cat?.title || h?.id || m.name)}</h4><div class="meta">${esc(m.name)}${h ? ' · v' + esc(h.version) + ' · ' + (h.image / 1024).toFixed(1) + ' KB' : ''}${err && cls === 'err' ? ' · ' + esc(err) : ''}</div></div>
      <span class="tag ${cls}">${label}</span>
      <button type="button" class="switch" role="switch" aria-checked="${m.on}" aria-label="Load at boot" data-toggle="${esc(m.name)}"></button>
      <button type="button" class="btn danger small" data-remove="${esc(m.name)}">Remove</button></li>`;
  }).join('');
}

// ---- the theme ------------------------------------------------------------------------------------------------------
let liveTimer = null, liveBusy = false, liveAgain = false;
function setTheme(colors, name, { edited = true, push = true } = {}) {
  state.theme = { colors: Uint8Array.from(colors), name: (name || '').toUpperCase().slice(0, 12) };
  if (edited) state.themeEdited = true;
  store.set('dl.theme', { colors: hexOf(state.theme.colors), name: state.theme.name });
  applyTokens(state.theme.colors);
  renderTheme();
  if (push) pushLive();
}
function setExt(flags, rgb, { edited = true, push = true } = {}) {
  state.ext = { flags: flags & 7, rgb: Uint8Array.from(rgb) };
  if (edited) state.extEdited = true;
  store.set('dl.themeExt', { flags: state.ext.flags, rgb: hexOf(state.ext.rgb) });
  renderTheme();
  if (push) pushLive();
}
function pushLive() {
  if (!state.m8 || !state.themeOk || !$('#live').checked) return;
  clearTimeout(liveTimer);
  liveTimer = setTimeout(async () => {
    if (liveBusy) { liveAgain = true; return; }
    liveBusy = true;
    try {
      await state.m8.setTheme(state.theme.colors);
      if (state.extOk) await state.m8.setThemeExt(state.ext.flags, state.ext.rgb);
      state.liveDirty = true; $('#revert').disabled = false;
    }
    catch (e) { /* unplugged mid-drag: the next connect sorts it out */ }
    liveBusy = false;
    if (liveAgain) { liveAgain = false; pushLive(); }
  }, 60);
}
async function loadDeviceThemes() {
  state.deviceTheme = null; state.deviceThemes = []; state.themeOk = false; state.deviceExt = null; state.extOk = false;
  if (!state.m8) return;
  try { state.deviceExt = await state.m8.themeExt(); state.extOk = true; }
  catch (e) { if (!(e instanceof UnitError && e.status === 7)) throw e; }   // a core without THEMEX
  if (state.deviceExt && !state.extEdited) setExt(state.deviceExt.flags, state.deviceExt.rgb, { edited: false, push: false });
  try {
    state.deviceTheme = await state.m8.theme();
    state.themeOk = true;
  } catch (e) {
    if (!(e instanceof UnitError && e.status === 7)) throw e;   // an older core: no THEME command
  }
  try {
    for (const f of (await state.m8.list('/Themes')).filter(n => n.toLowerCase().endsWith('.m8t')).sort()) {
      try { state.deviceThemes.push({ name: f.replace(/\.m8t$/i, '').toUpperCase(), colors: parseM8t(await state.m8.get('/Themes/' + f)), file: f }); } catch { /* not a theme */ }
    }
  } catch { /* no /Themes folder */ }
  // the page takes the M8's own theme, unless you were already making one
  if (state.deviceTheme && !state.themeEdited) setTheme(state.deviceTheme.colors, state.deviceTheme.name || 'MY M8', { edited: false, push: false });
}
// The theme dropdown: the M8's (what it shows now, its /Themes), the shop's, the ones kept in this browser.
let pickList = [], pickSig = '', pickAt = -1, pickActive = -1, pickChosen = -1;
const pickInner = (p) => `<span class="tp-name">${esc(p.name)}</span><span class="tp-note">${esc(p.note || '')}</span>
  <span class="sw" aria-hidden="true">${[0, 5, 7, 6, 8, 12].map(i => `<i style="background:${toHex(p.colors, i)}"></i>`).join('')}</span>`;
function pickTheme(k) {
  const p = pickList[k];
  if (!p) return;
  pickChosen = k;   // two entries can hold the same theme (a preset and its copy on the card): show the one picked
  if (p.ext) setExt(p.ext.flags, bytesOf(p.ext.rgb), { push: false });
  setTheme(p.colors, p.name);
}
function pickOpen(open) {
  const list = $('#tpick-list'), btn = $('#tpick-btn');
  if (open === !list.hidden) return;
  list.hidden = !open; btn.setAttribute('aria-expanded', String(open));
  if (open) { pickMove(Math.max(pickAt, 0)); list.focus(); }
  else list.removeAttribute('aria-activedescendant');
}
function pickMove(k) {
  const list = $('#tpick-list');
  pickActive = Math.min(Math.max(k, 0), pickList.length - 1);
  for (const li of list.querySelectorAll('.active')) li.classList.remove('active');
  const li = $('#tp-' + pickActive);
  if (li) { li.classList.add('active'); list.setAttribute('aria-activedescendant', li.id); li.scrollIntoView({ block: 'nearest' }); }
}
function wirePicker() {
  const list = $('#tpick-list'), btn = $('#tpick-btn');
  btn.onclick = () => pickOpen(list.hidden);
  btn.onkeydown = (e) => { if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); pickOpen(true); } };
  list.onclick = (e) => { const li = e.target.closest('[role=option]'); if (li) { pickTheme(+li.dataset.k); pickOpen(false); btn.focus(); } };
  list.onmousemove = (e) => { const li = e.target.closest('[role=option]'); if (li && +li.dataset.k !== pickActive) pickMove(+li.dataset.k); };
  list.onkeydown = (e) => {
    const step = { ArrowDown: 1, ArrowUp: -1, PageDown: 6, PageUp: -6 }[e.key];
    if (step) { e.preventDefault(); pickMove(pickActive + step); }
    else if (e.key === 'Home' || e.key === 'End') { e.preventDefault(); pickMove(e.key === 'Home' ? 0 : pickList.length - 1); }
    else if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pickTheme(pickActive); pickOpen(false); btn.focus(); }
    else if (e.key === 'Escape') { e.preventDefault(); pickOpen(false); btn.focus(); }
    else if (e.key === 'Tab') pickOpen(false);
  };
  document.addEventListener('pointerdown', (e) => { if (!list.hidden && !e.target.closest('#tpick')) pickOpen(false); });
}
function renderPicker() {
  const groups = [];
  if (state.m8 && state.themeOk) groups.push(['From your M8', [
    ...(state.deviceTheme ? [{ colors: state.deviceTheme.colors, name: state.deviceTheme.name || 'ON THE M8', note: 'on the M8 now' }] : []),
    ...state.deviceThemes.map(t => ({ ...t, note: '/Themes/' + t.file })),
  ]]);
  groups.push(['Here', PRESETS.map(p => ({ ...p, note: p.from === 'm8' ? "the M8's default" : '' }))]);
  if (state.mine.length) groups.push(['Kept in this browser', state.mine.map(p => ({ name: p.name, colors: bytesOf(p.colors), ext: p.ext, note: p.ext?.flags ? '+ more colours' : '' }))]);
  const list = $('#tpick-list');
  const sig = JSON.stringify(groups.map(([g, l]) => [g, l.map(p => p.name + p.note + hexOf(p.colors))]));
  if (sig !== pickSig) {   // rebuild only when the list changes, not on every colour edit
    pickSig = sig; pickList = [];
    list.innerHTML = groups.map(([g, l]) => `<li class="tp-group" role="presentation">${esc(g)}</li>${l.map(p => {
      pickList.push(p); const k = pickList.length - 1;
      return `<li role="option" id="tp-${k}" data-k="${k}" aria-selected="false">${pickInner(p)}</li>`; }).join('')}`).join('');
  }
  const cur = hexOf(state.theme.colors);
  const k = pickList.findIndex(p => hexOf(p.colors) === cur && p.name === state.theme.name);
  const chosen = pickList[pickChosen];
  pickAt = chosen && hexOf(chosen.colors) === cur && chosen.name === state.theme.name ? pickChosen
    : k >= 0 ? k : pickList.findIndex(p => hexOf(p.colors) === cur);
  for (const li of list.querySelectorAll('[role=option]')) li.setAttribute('aria-selected', String(+li.dataset.k === pickAt));
  $('#tpick-btn').innerHTML = pickInner(pickAt >= 0 ? pickList[pickAt] : { name: state.theme.name || 'UNTITLED', note: 'edited', colors: state.theme.colors })
    + '<span class="chev" aria-hidden="true">▾</span>';
  $('#theme-pick-note').textContent = !state.m8 ? 'Connect your M8 to pick its own themes too.'
    : !state.themeOk ? "This core is older than 0.7: it can't read or set the theme." : '';
}
function renderTheme() {
  if (!state.theme) return;
  renderPicker();
  if (document.activeElement !== $('#theme-name')) $('#theme-name').value = state.theme.name;
  drawScreen($('#screen'), state.theme.colors, { glow: glowSlot, phase: glowPhase, scale: 2, ext: state.ext });
  renderExt();
  const slots = $('#slots');
  if (!slots.children.length) {
    slots.innerHTML = SLOTS.map((s, i) => `<li class="slot" data-slot="${i}"><input type="color" aria-label="${s.label}" data-color="${i}">
      <div><b>${s.label}</b><span>${s.hint}</span></div><input class="hex" aria-label="${s.label} as hex" data-hex="${i}" maxlength="7" spellcheck="false"></li>`).join('');
  }
  for (let i = 0; i < 13; i++) {
    const hex = toHex(state.theme.colors, i);
    const c = slots.querySelector(`[data-color="${i}"]`), h = slots.querySelector(`[data-hex="${i}"]`);
    if (c.value !== hex) c.value = hex;
    if (document.activeElement !== h) h.value = hex.toUpperCase();
  }
  $('#apply').disabled = !(state.m8 && state.themeOk);
  $('#live').disabled = !(state.m8 && state.themeOk);
  $('#theme-note').textContent = state.m8 && state.themeOk ? 'Apply saves it on the M8 like the THEME page does, so it stays after a power-off.'
    : 'Connect your M8 (with core 0.7) to see it live and apply it. You can still make one here and download the .m8t.';
}
// hovering a colour blinks what it paints on the screen
let glowSlot = -1, glowPhase = 0, glowTimer = null;
function glow(i) {
  glowSlot = i; clearInterval(glowTimer);
  if (i < 0) { glowPhase = 0; renderScreenOnly(); return; }
  glowTimer = setInterval(() => { glowPhase ^= 1; renderScreenOnly(); }, 380);
}
function renderScreenOnly() { if (state.theme) drawScreen($('#screen'), state.theme.colors, { glow: glowSlot, phase: glowPhase, scale: 2, ext: state.ext }); }
function renderExt() {
  const list = $('#ext-slots');
  if (!list.children.length) {
    list.innerHTML = EXT_SLOTS.map((s, k) => `<li class="slot" data-slot="${13 + k}"><input type="color" aria-label="${s.label}" data-xcolor="${k}">
      <div><b>${s.label}</b><span>${s.hint}</span></div><input class="hex" aria-label="${s.label} as hex" data-xhex="${k}" maxlength="7" spellcheck="false"></li>`).join('');
  }
  for (let k = 0; k < EXT_SLOTS.length; k++) {
    const hex = toHex(state.ext.rgb, k);
    const c = list.querySelector(`[data-xcolor="${k}"]`), h = list.querySelector(`[data-xhex="${k}"]`);
    if (c.value !== hex) c.value = hex;
    if (document.activeElement !== h) h.value = hex.toUpperCase();
  }
  for (const box of document.querySelectorAll('#ext [data-flag]')) box.checked = !!(state.ext.flags & +box.dataset.flag);
  $('#ext').classList.toggle('off', !state.ext.flags);
  $('#ext-note').textContent = state.m8 && !state.extOk
    ? "This M8's core is older: it doesn't have these colours. Update core to use them."
    : 'Track colours paint SONG\'s track columns and the track list beside every screen; titles give each screen its own; the cursor, selection and play markers keep the theme\'s colours. Apply keeps them on the card (/dirtyloader/theme.ext). A .m8t file holds only the 13 colours.';
}

// ---- the frame ------------------------------------------------------------------------------------------------------
function route() {
  const v = (location.hash || '#shop').slice(1).split('?')[0];
  const views = ['start', 'shop', 'device', 'theme', 'recover'];
  const view = views.includes(v) ? v : 'shop';
  for (const k of views) {
    $('#view-' + k).hidden = k !== view;
    $('#nav-' + k).classList.toggle('on', k === view);
    $('#tab-' + k).classList.toggle('on', k === view);
  }
  document.title = { start: 'Get started', shop: 'Mod Shop', device: 'On your M8', theme: 'Theme', recover: 'Recover' }[view] + ' · dirtyloader';
  $('#content').scrollTop = 0;
}
function renderAll() {
  renderSide(); renderChips(); renderShelves(); renderDeviceBox(); renderDevice(); renderTheme(); renderStart();
}

function wire() {
  window.addEventListener('hashchange', route);
  document.addEventListener('click', async (e) => {
    const t = e.target.closest('button, [data-cat]');
    if (!t) return;
    if (t.dataset.cat) { state.cat = t.dataset.cat; location.hash = '#shop'; renderSide(); renderShelves(); }
    else if (t.dataset.target) { state.target = t.dataset.target; store.set('dl.target', state.target); renderChips(); renderShelves(); }
    else if (t.dataset.install) install(state.catalog.mods.find(m => m.id === t.dataset.install));
    else if (t.dataset.details) details(t.dataset.details);
    else if (t.dataset.goto) { location.hash = '#device'; setTimeout(() => { const r = document.querySelector(`[data-row="${CSS.escape(t.dataset.goto)}"]`); if (r) { r.classList.add('flash'); r.scrollIntoView({ block: 'center' }); setTimeout(() => r.classList.remove('flash'), 1600); } }, 50); }
    else if (t.dataset.toggle) {
      const m = state.mods.find(x => x.name === t.dataset.toggle);
      try { await state.m8.setOn(m.name, !m.on); await refreshDevice(); renderAll(); } catch (er) { toast(er.message, { kind: 'bad' }); }
    } else if (t.dataset.remove) {
      if (!confirm(`Delete ${t.dataset.remove} from your M8's card?`)) return;
      try { await state.m8.uninstall(t.dataset.remove); await refreshDevice(); renderAll(); toast(`${t.dataset.remove} deleted from the card.`); } catch (er) { toast(er.message, { kind: 'bad' }); }
    } else if ('connect' in t.dataset || t.id === 'g-connect-btn') connect();
    else if (t.id === 'pending-restart') restart();
  });
  $('#connect').onclick = async () => { if (state.m8) { state.reconnect = null; const m = state.m8; state.m8 = null; state.info = null; state.mods = []; await m.close(); renderAll(); } else connect(); };
  $('#restart').onclick = restart;
  $('#refresh').onclick = async () => { await refreshDevice(); await loadDeviceThemes(); renderAll(); };
  $('#about-open').onclick = () => $('#about').showModal();
  $('#search').oninput = (e) => { state.query = e.target.value; if (location.hash !== '#shop' && location.hash) location.hash = '#shop'; renderShelves(); };
  $('#f-sort').onchange = (e) => { state.sort = e.target.value; renderShelves(); };
  $('#show-dev').checked = state.showDev;
  $('#show-dev').onchange = (e) => { state.showDev = e.target.checked; store.set('dl.dev', state.showDev); renderSide(); renderChips(); renderShelves(); };
  for (const id of ['#add-own', '#add-own-2']) $(id).onclick = () => $('#own-files').click();
  $('#own-files').onchange = (e) => { installOwn([...e.target.files]); e.target.value = ''; };
  const drop = $('#drop');
  drop.onclick = () => $('#own-files').click();
  drop.ondragover = (e) => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = (e) => { e.preventDefault(); drop.classList.remove('over'); installOwn([...e.dataTransfer.files].filter(f => f.name.toLowerCase().endsWith('.m8x'))); };

  // theme
  $('#slots').addEventListener('input', (e) => {
    const i = +(e.target.dataset.color ?? e.target.dataset.hex);
    if (Number.isNaN(i)) return;
    let v = e.target.value.trim();
    if (e.target.dataset.hex !== undefined) { if (!/^#?[0-9a-f]{6}$/i.test(v)) return; if (v[0] !== '#') v = '#' + v; }
    const c = Uint8Array.from(state.theme.colors);
    setHex(c, i, v);
    setTheme(c, state.theme.name);
  });
  $('#ext-slots').addEventListener('input', (e) => {
    const k = +(e.target.dataset.xcolor ?? e.target.dataset.xhex);
    if (Number.isNaN(k)) return;
    let v = e.target.value.trim();
    if (e.target.dataset.xhex !== undefined) { if (!/^#?[0-9a-f]{6}$/i.test(v)) return; if (v[0] !== '#') v = '#' + v; }
    const rgb = Uint8Array.from(state.ext.rgb);
    setHex(rgb, k, v);
    setExt(state.ext.flags || 1, rgb);                // editing a colour switches the feature on
  });
  $('#ext').addEventListener('change', (e) => {
    const f = e.target.dataset.flag;
    if (f) setExt(e.target.checked ? state.ext.flags | +f : state.ext.flags & ~+f, state.ext.rgb);
  });
  $('#ext').addEventListener('click', (e) => {
    const b = e.target.closest('[data-fill]');
    if (b) setExt(state.ext.flags || 3, bytesOf(EXT_PRESETS[b.dataset.fill]));
  });
  $('#ext-slots').addEventListener('mouseover', (e) => { const s = e.target.closest('.slot'); if (s && +s.dataset.slot !== glowSlot) glow(+s.dataset.slot); });
  $('#ext-slots').addEventListener('mouseleave', () => glow(-1));
  $('#slots').addEventListener('mouseover', (e) => { const s = e.target.closest('.slot'); if (s && +s.dataset.slot !== glowSlot) glow(+s.dataset.slot); });
  $('#slots').addEventListener('mouseleave', () => glow(-1));
  $('#slots').addEventListener('focusin', (e) => { const s = e.target.closest('.slot'); if (s) glow(+s.dataset.slot); });
  $('#theme-name').oninput = (e) => { state.theme.name = e.target.value.toUpperCase().replace(/[^A-Z0-9 _-]/g, '').slice(0, 12); store.set('dl.theme', { colors: hexOf(state.theme.colors), name: state.theme.name }); };
  $('#live').onchange = () => { if ($('#live').checked) pushLive(); };
  $('#apply').onclick = async () => {
    try {
      await state.m8.setTheme(state.theme.colors, { save: true, name: state.theme.name || 'DIRTYLOADER' });
      if (state.extOk) { await state.m8.setThemeExt(state.ext.flags, state.ext.rgb, { save: true }); state.deviceExt = await state.m8.themeExt(); }
      state.deviceTheme = await state.m8.theme(); state.liveDirty = false; $('#revert').disabled = true;
      toast(`Your M8 is now in ${state.theme.name || 'this theme'}: kept after a power-off.`);
      renderTheme();
    } catch (e) { toast(e.message, { kind: 'bad' }); }
  };
  $('#revert').onclick = async () => {
    if (!state.deviceTheme) return;
    try {
      // the firmware can save its settings on its own while you preview: put the old ones back for good
      await state.m8.setTheme(state.deviceTheme.colors, { save: true, name: state.deviceTheme.name });
      if (state.extOk && state.deviceExt) {
        await state.m8.setThemeExt(state.deviceExt.flags, state.deviceExt.rgb);
        setExt(state.deviceExt.flags, state.deviceExt.rgb, { push: false });
      }
      setTheme(state.deviceTheme.colors, state.deviceTheme.name, { push: false });
      state.liveDirty = false; $('#revert').disabled = true;
      toast("Your M8's theme is back.");
    } catch (e) { toast(e.message, { kind: 'bad' }); }
  };
  $('#save-mine').onclick = () => {
    const name = state.theme.name || 'MY THEME';
    state.mine = [{ name, colors: hexOf(state.theme.colors), ext: { flags: state.ext.flags, rgb: hexOf(state.ext.rgb) } },
      ...state.mine.filter(p => p.name !== name)].slice(0, 24);
    store.set('dl.mythemes', state.mine); renderTheme(); toast(`${name} kept in this browser.`);
  };
  $('#download').onclick = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([encodeM8t(state.theme.colors)], { type: 'application/octet-stream' }));
    a.download = (state.theme.name || 'THEME').replace(/[^A-Z0-9_-]/gi, '_') + '.m8t';
    a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  };
  wirePicker();
  $('#open-m8t').onclick = () => $('#m8t-file').click();
  $('#m8t-file').onchange = async (e) => {
    const f = e.target.files[0]; e.target.value = '';
    if (!f) return;
    try { setTheme(parseM8t(new Uint8Array(await f.arrayBuffer())), f.name.replace(/\.m8t$/i, '')); }
    catch (er) { toast(er.message, { kind: 'bad' }); }
  };
  window.addEventListener('beforeunload', (e) => { if (state.liveDirty) { e.preventDefault(); e.returnValue = ''; } });
  if ('serial' in navigator) {
    navigator.serial.addEventListener('connect', (e) => { if (state.reconnect && !state.m8) connect(e.target, { quiet: true }); });
  }
}

async function main() {
  const saved = store.get('dl.theme', null);
  const start = saved && /^[0-9a-f]{78}$/.test(saved.colors) ? { c: bytesOf(saved.colors), n: saved.name } : { c: PRESETS[0].colors, n: 'DEFAULT' };
  state.themeEdited = !!saved;
  state.theme = { colors: start.c, name: start.n };
  const xs = store.get('dl.themeExt', null);
  state.ext = xs && /^[0-9a-f]{90}$/.test(xs.rgb) ? { flags: xs.flags & 7, rgb: bytesOf(xs.rgb) } : { flags: 0, rgb: bytesOf(EXT_PRESETS.RAINBOW) };
  state.extEdited = !!xs;
  applyTokens(state.theme.colors);
  try {
    state.catalog = await (await fetch('catalog.json', { cache: 'no-cache' })).json();
  } catch (e) {
    $('#shelves').innerHTML = `<p class="empty">The mod list didn't load (${esc(e.message)}).</p>`;
    return;
  }
  if (state.target !== 'all' && !state.catalog.targets[state.target]) state.target = 'all';
  wire(); route();
  await initGuide({ state, store, connect, restart, toast, renderAll, reconnectSoon });
  renderAll();
  if (new URLSearchParams(location.search).has('demo')) { await demo(); return; }
  if (!('serial' in navigator)) {
    $('#g-connect-text').innerHTML = "This browser can't talk to USB devices (Web Serial). Use Chrome, Edge or Opera on a computer to install mods and set the theme; you can still browse and make themes here.";
    $('#connect').disabled = true; $('#g-connect-btn').hidden = true;
  } else {
    // a port this page was allowed before: connect to it quietly
    const ports = (await navigator.serial.getPorts()).filter(p => p.getInfo().usbVendorId === TEENSY_VID);
    if (ports.length === 1) connect(ports[0]).catch(() => {});
  }
}
// ?demo: a pretend Model:02 (site/shop/test/fake_m8.js) with two mods and three themes on its card, to try the page
// without an M8. Nothing leaves the page.
async function demo() {
  const { FakeCore, fakePort } = await import('./test/fake_m8.js');
  const files = { '/dirtyloader/active.txt': ['chaos-control.m8x', 'perf-mon.m8x', ''].join(String.fromCharCode(10)) };
  for (const id of ['chaos-control', 'perf-mon']) {
    const b = state.catalog.mods.find(m => m.id === id).builds['m8-model02'];
    files['/dirtyloader/mods/' + id + '.m8x'] = new Uint8Array(await (await fetch(b.file)).arrayBuffer());
  }
  for (const p of PRESETS.slice(2, 5)) files['/Themes/' + p.name + '.m8t'] = encodeM8t(p.colors);
  const port = fakePort(new FakeCore({ loaded: ['chaos-control', 'perf-mon'], files }));
  state.demo = port.core;
  const bar = document.createElement('p');
  bar.className = 'demo-bar';
  bar.textContent = 'Demo: a pretend M8 Model:02 in this page. Nothing is sent anywhere.';
  $('.topbar').prepend(bar);
  await connect(port);
}
window.dlShop = { connect, state };   // for tests (site/shop/test) and the console
main();
