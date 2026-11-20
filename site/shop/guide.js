// "Get started" and "Recover": build core into Dirtywave's firmware here, flash it over USB (halfkay.js), and get
// back to stock. app.js hands in what it shares (state, connect, restart, toast).
import { identify, buildFirmware, parseBin, parseHex, zipEntries, describeStock } from './firmware.js';
import { flashImage, hidSupported, knownBootloader, chooseBootloader, openBootloader, rebootToBootloader, BLOCK } from './halfkay.js';
import { TEENSY_VID } from './m8.js';

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const mb = (n) => (n / 1048576).toFixed(2) + ' MB';

let ctx, index = [];
const g = { model: null, stock: null, build: null, urls: [], rec: null, busy: false };

export async function initGuide(c) {
  ctx = c;
  try { index = await (await fetch('firmware/index.json', { cache: 'no-cache' })).json(); } catch { index = []; }
  g.model = ctx.store.get('dl.fwModel', null) || 'm8-model02-664';   // Dirtywave's current download
  if (!index.some(e => e.target === g.model)) g.model = index[0]?.target || null;
  wireDrop('#fw-drop', '#fw-file', takeStock);
  wireDrop('#rec-drop', '#rec-file', takeRecovery);
  $('#fw-models').onclick = (e) => { const b = e.target.closest('[data-model]'); if (b && !b.disabled) setModel(b.dataset.model); };
  $('#fw-build').onclick = build;
  $('#fw-ack').onchange = renderStart;
  $('#fw-flash').onclick = () => flash(g.build?.image, 'fw', false);
  $('#fw-flash-bl').onclick = () => flash(g.build?.image, 'fw', true);
  $('#rec-flash').onclick = () => flash(g.rec?.image, 'rec', false);
  $('#rec-flash-bl').onclick = () => flash(g.rec?.image, 'rec', true);
  $('#rec-skip').onclick = () => guard('SKIP');
  $('#rec-good').onclick = () => guard('GOOD');
  renderStart();
}

/** The M8 that's connected tells us its model. */
export function guideSawDevice(info) {
  if (g.expectSkip && Date.now() < g.expectSkip) {          // core lifts SKIP on a restart in a boot's first 10 s
    g.expectSkip = 0;
    if (info?.loaded?.length) ctx.toast('The M8 had only just started, so core kept the mods. Wait 10 s and press Restart without mods again.', { kind: 'warn', ms: 12000 });
  }
  if (info?.target && index.some(e => e.target === info.target) && !g.stock) setModel(info.target);
  else renderStart();
}

function wireDrop(drop, input, take) {
  const d = $(drop), f = $(input);
  d.onclick = () => f.click();
  d.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); f.click(); } };
  d.ondragover = (e) => { e.preventDefault(); d.classList.add('over'); };
  d.ondragleave = () => d.classList.remove('over');
  d.ondrop = (e) => { e.preventDefault(); d.classList.remove('over'); if (e.dataTransfer.files[0]) take(e.dataTransfer.files[0]); };
  f.onchange = () => { if (f.files[0]) take(f.files[0]); f.value = ''; };
}

function setModel(m) {
  g.model = m; ctx.store.set('dl.fwModel', m);
  if (g.stock && g.stock.entry.target !== m) { g.stock = null; clearBuild(); }
  renderStart();
}
const entry = () => index.find(e => e.target === g.model);

// ---- step 2: Dirtywave's file ----
async function takeStock(file) {
  clearBuild();
  const note = $('#fw-stock-note');
  note.className = 'step-note'; note.textContent = `Reading ${file.name}...`;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const hit = await identify(bytes, file.name, index);
    if (hit.entry) {
      g.stock = hit;
      if (hit.entry.target !== g.model) { g.model = hit.entry.target; ctx.store.set('dl.fwModel', g.model); }
      note.className = 'step-note ok';
      note.textContent = `✓ ${hit.name}: Dirtywave's ${hit.entry.label} ${hit.entry.firmware}, recognised by its sha256.`;
    } else {
      g.stock = null;
      const e = entry();
      const what = hit.candidates.length ? hit.candidates.map(c => c.name.split('/').pop()).join(', ') : file.name;
      note.className = 'step-note bad';
      note.textContent = `✗ ${what}: not a firmware core is built for${e ? `. Core needs Dirtywave's ${e.label} ${e.firmware} (${e.stock_name}), unchanged.` : '.'}`;
    }
  } catch (e) {
    g.stock = null; note.className = 'step-note bad'; note.textContent = `✗ ${file.name}: ${e.message}`;
  }
  renderStart();
}

// ---- step 3: build ----
function clearBuild() {
  for (const u of g.urls) URL.revokeObjectURL(u);
  g.urls = []; g.build = null;
  $('#fw-result').innerHTML = '';
}
async function build() {
  if (!g.stock) return;
  clearBuild();
  const btn = $('#fw-build');
  btn.disabled = true; btn.textContent = 'Building...';
  const t0 = performance.now();
  try {
    const patch = await (await fetch(g.stock.entry.file, { cache: 'no-cache' })).json();
    const { image, files } = await buildFirmware(patch, g.stock.bytes);
    const ours = patch.ops.filter(o => o.length === 2).reduce((a, o) => a + atob(o[1]).length, 0);
    g.build = { image, files, patch };
    const links = files.map(f => {
      const url = URL.createObjectURL(new Blob([f.data], { type: 'application/octet-stream' }));
      g.urls.push(url);
      const use = f.kind === 'bin' ? 'for the SD card' : 'for TyUploader';
      return `<a class="dl" data-gated href="${url}" download="${esc(f.name)}"><span>Download ${esc(f.name)}</span><span class="muted small">${use} · ${mb(f.data.length)} · sha256 ${f.sha256.slice(0, 16)}…</span></a>`;
    }).join('');
    $('#fw-result').innerHTML = `<p class="step-note ok">✓ Built and checked, in ${Math.round(performance.now() - t0)} ms in this tab.</p>
      <dl class="facts"><dt>M8</dt><dd>${esc(patch.label)}</dd><dt>Firmware</dt><dd>Dirtywave's ${esc(patch.firmware)}, with dirtyloader core ${esc(patch.core)}</dd>
      <dt>Image</dt><dd>${mb(image.length)} (Dirtywave's: ${mb(patch.stock.payload)}; core sits past the end of it)</dd>
      <dt>From dirtyloader</dt><dd>${ours.toLocaleString()} bytes; everything else is your file's</dd>
      <dt>sha256</dt><dd class="mono">${esc(patch.out.sha256)}</dd></dl>
      <div class="downloads">${links}</div>`;
    $('#fw-result').onclick = (e) => { if (e.target.closest('[data-gated]') && !$('#fw-ack').checked) { e.preventDefault(); ctx.toast('Tick "I know how to get my M8 back to stock" first (step 4).', { kind: 'warn' }); } };
  } catch (e) {
    $('#fw-result').innerHTML = `<p class="step-note bad">✗ Not built: ${esc(e.message)}</p>`;
  }
  btn.textContent = 'Build firmware with core';
  renderStart();
}

// ---- step 4 / recover: flash over USB ----
function progressUI(which, p, phase) {
  const box = $(`#${which}-progress`);
  box.hidden = false;
  box.querySelector('i').style.width = Math.round(p * 100) + '%';
  $(`#${which}-progress-text`).textContent = phase;
}
function noteUI(which, text, kind = '') {
  const n = which === 'fw' ? $('#fw-usb-note') : $('#rec-stock-note');
  n.className = 'step-note ' + kind; n.textContent = text; delete n.dataset.auto;
}

class DemoBootloader {          // ?demo: a pretend bootloader, nothing is sent
  constructor() { this.n = 0; }
  async sendReport() { if (++this.n === 3) await sleep(1500); else if (this.n % 64 === 0) await sleep(1); }
}

async function serialPortForReboot() {
  if (ctx.state.m8) {                                   // the shop is connected: let go of the port first
    const m8 = ctx.state.m8, port = m8.t.port;
    ctx.state.reconnect = null; ctx.state.m8 = null; ctx.state.info = null; ctx.state.mods = [];
    try { await m8.close(); } catch { /* gone */ }
    ctx.renderAll();
    return port;
  }
  const ports = (await navigator.serial.getPorts()).filter(p => p.getInfo().usbVendorId === TEENSY_VID);
  if (ports.length === 1) return ports[0];
  return navigator.serial.requestPort({ filters: [{ usbVendorId: TEENSY_VID }] });
}

async function flash(image, which, already) {
  if (!image || g.busy) return;
  const label = which === 'fw' ? 'core' : 'Dirtywave\'s firmware';
  g.busy = true; renderStart();
  try {
    let dev;
    if (ctx.state.demo) {
      progressUI(which, 0, 'Restarting the M8 into its bootloader (demo)...'); await sleep(600);
      dev = new DemoBootloader();
    } else {
      if (!hidSupported()) throw new Error('This browser can\'t flash (no WebHID): use Chrome or Edge on a computer, or TyUploader with the .hex.');
      if (!already) {
        progressUI(which, 0, 'Restarting the M8 into its bootloader...');
        await rebootToBootloader(await serialPortForReboot());
        for (let t = 0; t < 12 && !dev; t++) { await sleep(200); dev = await knownBootloader(); }
      } else dev = await knownBootloader();
      if (!dev) {
        progressUI(which, 0, 'Pick the bootloader (Teensy / HalfKay) in the browser\'s list.');
        try { dev = await chooseBootloader(); } catch (e) {
          if (e.name === 'NotFoundError') throw new Error('No bootloader was picked. If the M8 restarted into it, press "It\'s already in its bootloader".');
          if (e.name === 'SecurityError' || e.name === 'NotAllowedError') throw new Error('The browser wants another click to show the bootloader: press "It\'s already in its bootloader".');
          throw e;
        }
        if (!dev) throw new Error('No bootloader was picked.');
      }
      await openBootloader(dev);
    }
    const t0 = Date.now();
    await flashImage(dev, image, {
      onProgress: (p, phase) => progressUI(which, p, phase === 'erasing' ? 'Erasing the flash (up to a minute)...'
        : p >= 1 ? 'Done' : `Writing ${Math.round(p * 100)}% · ${Math.round(image.length * p / 1024)} of ${Math.round(image.length / 1024)} KB`),
    });
    try { await dev.close?.(); } catch { /* it's rebooting */ }
    progressUI(which, 1, `Flashed in ${Math.round((Date.now() - t0) / 1000)} s (${Math.ceil(image.length / BLOCK)} blocks).`);
    noteUI(which, which === 'fw'
      ? `✓ The M8 is restarting with ${label}. Connect it (top right) to check: the sidebar shows core's version.`
      : `✓ The M8 is restarting with ${label}.`, 'ok');
    if (which === 'fw' && !ctx.state.demo) ctx.reconnectSoon();
  } catch (e) {
    noteUI(which, `✗ ${e.message || e}`, 'bad');
    ctx.toast(e.message || String(e), { kind: 'bad', ms: 10000 });
  } finally {
    g.busy = false; renderStart();
  }
}

// ---- recover: Dirtywave's file, as it is ----
async function takeRecovery(file) {
  const note = $('#rec-stock-note');
  g.rec = null;
  note.className = 'step-note'; note.textContent = `Reading ${file.name}...`;
  try {
    let bytes = new Uint8Array(await file.arrayBuffer()), name = file.name;
    const known = await identify(bytes, name, index);
    if (known.entry) {
      g.rec = { image: known.entry.kind === 'bin' ? parseBin(known.bytes).payload : parseHex(new TextDecoder('latin1').decode(known.bytes)).data,
        what: `Dirtywave's ${known.entry.label} ${known.entry.firmware} (${known.name}), recognised by its sha256` };
    } else {
      if (bytes[0] === 0x50 && bytes[1] === 0x4B) {        // a .zip: the file for the M8 picked on Get started
        const want = g.model === 'm8-headless' ? /\.hex$/i : /Model02\/[^/]*\.bin$/i;
        const z = zipEntries(bytes).find(x => !x.name.includes('__MACOSX') && want.test(x.name));
        if (!z) throw new Error(`no ${g.model === 'm8-headless' ? 'Headless .hex' : 'Model:02 .bin'} in it`);
        bytes = await z.read(); name = z.name.split('/').pop();
      }
      if (bytes[0] === 0x3A) {
        const h = parseHex(new TextDecoder('latin1').decode(bytes));
        if (h.base !== 0x60000000) throw new Error('this .hex is not for the M8\'s flash');
        g.rec = { image: h.data, what: `${name}: a firmware .hex for the M8's flash (a Headless's; only its records were checked)` };
      } else {
        const b = parseBin(bytes);
        g.rec = { image: b.payload, what: `${describeStock(bytes, name)}, checksums good` };
      }
    }
    note.className = 'step-note ok'; note.textContent = '✓ ' + g.rec.what + '.';
  } catch (e) {
    note.className = 'step-note bad'; note.textContent = `✗ ${file.name}: ${e.message}`;
  }
  renderStart();
}

async function guard(word) {
  if (!ctx.state.m8) { await ctx.connect(); if (!ctx.state.m8) return; }
  try {
    if (ctx.state.demo) { ctx.toast(`Demo: guard = ${word}, then a restart.`); return; }
    await ctx.state.m8.put('guard', new TextEncoder().encode(word));
    g.expectSkip = word === 'SKIP' ? Date.now() + 60000 : 0;
    ctx.toast(word === 'SKIP' ? 'Restarting without mods. The MODS page will say SKIPPED.' : 'Restarting with your mods.');
    await ctx.restart();
  } catch (e) { ctx.toast(e.message || String(e), { kind: 'bad' }); }
}

// ---- render ----
export function renderStart() {
  if (!$('#fw-models')) return;
  $('#fw-models').innerHTML = index.map(e => `<button type="button" class="chip ${e.target === g.model ? 'on' : ''}" role="radio" aria-checked="${e.target === g.model}" data-model="${e.target}">${esc(e.label.replace('M8 ', ''))} <span class="n">${esc(e.firmware)}</span></button>`).join('')
    + '<button type="button" class="chip" disabled title="Model:01 firmware is built with the command-line tools and installed from SD">Model:01 <span class="n">SD setup</span></button>';
  const e = entry(), info = ctx.state.info;
  $('#fw-model-note').innerHTML = info?.target && index.some(x => x.target === info.target)
    ? `Your connected M8 is a ${esc(info.label)} on ${esc(info.version)}${info.core ? `, with core ${esc(info.core)} already` : ''}.`
    : 'Core is built for one firmware version per model.';
  $('#fw-get').innerHTML = e ? `Core is made for Dirtywave's <strong>${esc(e.label)} ${esc(e.firmware)}</strong>. Download
    <a href="${esc(e.get)}" target="_blank" rel="noreferrer">${esc(e.get_note)}</a>.${e.target === 'm8-model02'
      ? ' Core is also built for the Model:02\'s 6.6.4A (pick it above): use 6.6.2A only if you want to stay on it.'
      : e.target === 'm8-model02-664'
        ? ' That link is Dirtywave\'s latest: once they release a newer version, the file it gives won\'t be 6.6.4A, and this page will say so when you drop it in.' : ''}` : '';
  $('#st-model').classList.toggle('done', !!e);
  $('#st-stock').classList.toggle('done', !!g.stock);
  $('#fw-build').disabled = !g.stock || g.busy;
  $('#st-build').classList.toggle('done', !!g.build);
  const ack = $('#fw-ack').checked, ok = !!g.build && ack && !g.busy;
  $('#fw-flash').disabled = !ok; $('#fw-flash-bl').disabled = !ok;
  $('#st-flash').classList.toggle('dim', !g.build);
  for (const a of document.querySelectorAll('#fw-result [data-gated]')) a.setAttribute('aria-disabled', String(!ack));
  $('#way-sd').hidden = !String(g.model).startsWith('m8-model02');
  if (!$('#fw-usb-note').textContent || $('#fw-usb-note').dataset.auto) {
    $('#fw-usb-note').dataset.auto = '1';
    $('#fw-usb-note').textContent = !g.build ? 'Build first (step 3).' : !ack ? 'Tick the box above first.'
      : hidSupported() || ctx.state.demo ? '' : 'This browser can\'t flash (no WebHID): use Chrome or Edge on a computer, or TyUploader.';
  }
  $('#rec-flash').disabled = !g.rec || g.busy; $('#rec-flash-bl').disabled = !g.rec || g.busy;
  const on = !!ctx.state.m8;
  $('#rec-skip').disabled = $('#rec-good').disabled = !on;
  $('#rec-skip-note').textContent = on ? (ctx.state.info?.loaded?.length ? `Running now: ${ctx.state.info.loaded.join(', ')}.` : 'No mods are running now.')
    : 'Needs the M8 connected here (Connect M8, top right).';
}
