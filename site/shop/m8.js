// Talking to an M8 that runs dirtyloader core, over USB serial (Web Serial in the page; any byte transport in tests).
// The same protocol as python -m dirtyloader.unit (docs/LOADER.md, "Transfer"): a request is 0xD1, cmd, seq,
// len (u16 LE), payload, sum8 of cmd..payload; core answers with a SLIP frame 0xF1, cmd, seq, status, data.

export const CMD = { INFO: 1, LIST: 2, PUT: 3, CHECK: 4, GET: 5, MKDIRS: 6, RESTART: 7, PEEK: 8, DELETE: 9, POKE: 10,
  THEME: 11, THEMEX: 12 };
const STATUS = { 1: 'bad request', 2: 'checksum', 3: 'bad path or no such file', 4: 'SD card error', 5: 'no SD card',
  6: 'timeout', 7: 'this core does not know that command' };
const MARK = 0xD1, REPLY = 0xF1, CHUNK = 1024;
export const ACTIVE = 'active.txt';
export const TEENSY_VID = 0x16C0;

export class UnitError extends Error {
  constructor(msg, status) { super(msg); this.status = status; }
}

export function requestBytes(cmd, seq, payload = new Uint8Array()) {
  const out = new Uint8Array(6 + payload.length);
  out[0] = MARK; out[1] = cmd; out[2] = seq & 0xFF; out[3] = payload.length & 0xFF; out[4] = payload.length >> 8;
  out.set(payload, 5);
  let sum = 0;
  for (let i = 1; i < 5 + payload.length; i++) sum += out[i];
  out[5 + payload.length] = sum & 0xFF;
  return out;
}

/** Collects bytes and hands back each complete SLIP frame (m8c's screen frames are SLIP too: the caller sorts). */
export class Slip {
  constructor() { this.buf = []; this.esc = false; }
  push(bytes) {
    const frames = [];
    for (const b of bytes) {
      if (b === 0xC0) { if (this.buf.length) frames.push(Uint8Array.from(this.buf)); this.buf = []; this.esc = false; }
      else if (this.esc) { this.buf.push(b === 0xDC ? 0xC0 : b === 0xDD ? 0xDB : b); this.esc = false; }
      else if (b === 0xDB) this.esc = true;
      else this.buf.push(b);
    }
    return frames;
  }
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
export function crc32(bytes, crc = 0) {
  let c = (crc ^ 0xFFFFFFFF) >>> 0;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

const enc = new TextEncoder(), dec = new TextDecoder('latin1');
const le32 = (v) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
const cat = (...parts) => { const o = new Uint8Array(parts.reduce((n, p) => n + p.length, 0)); let i = 0; for (const p of parts) { o.set(p, i); i += p.length; } return o; };

/** Check a .m8x the way core does (python: dirtyloader.cc.parse_m8x) and read its header. Throws on a bad file. */
export function parseM8x(blob) {
  const b = blob instanceof Uint8Array ? blob : new Uint8Array(blob);
  const magic = dec.decode(b.subarray(0, 4));
  let hs, image, bss, nrel, crc, data = 0;
  if (magic === 'DLX2' && b.length >= 24) { image = u32(b, 4); bss = u32(b, 8); nrel = u32(b, 12); crc = u32(b, 16); data = u32(b, 20); hs = 24; }
  else if (magic === 'DLX1' && b.length >= 20) { image = u32(b, 4); bss = u32(b, 8); nrel = u32(b, 12); crc = u32(b, 16); hs = 20; }
  else throw new Error('not an .m8x file');
  const img = b.subarray(hs, hs + image), rel = b.subarray(hs + image, hs + image + 4 * nrel);
  if (img.length !== image || rel.length !== 4 * nrel) throw new Error('the .m8x is cut short');
  if (crc32(rel, crc32(img)) !== crc) throw new Error('the .m8x fails its checksum');
  if (u32(img, 0) !== 0x314D4C44) throw new Error('the .m8x does not start with a mod header');
  const cstr = (p) => { let e = p; while (e < img.length && img[e]) e++; return dec.decode(img.subarray(p, e)); };
  const nsubs = img[6] | img[7] << 8;
  const subs = [];
  for (let i = 0; i < nsubs; i++) { const o = u32(img, 20) + 8 * i; subs.push(img[o] | img[o + 1] << 8); }
  return { id: cstr(u32(img, 8)), version: cstr(u32(img, 12)), title: cstr(u32(img, 16)), fw: u32(img, 24),
    events: subs, size: b.length, bss, data };
}

/** Firmware ids as core reports them (and as .m8x files carry them): 0xMMvvvvvv, MM the model. */
export function describeFw(fw) {
  const model = fw >>> 24, v = fw & 0xFFFFFF;
  const ver = `${(v >> 16) & 0xFF}.${(v >> 8) & 0xFF}.${v & 0xFF}`;
  if (model === 1) return { target: v === 0x060604 ? 'm8-model01' : null, label: 'M8 Model:01', version: ver + 'A' };
  if (model === 2) return { target: v === 0x060604 ? 'm8-model02-664' : 'm8-model02', label: 'M8 Model:02', version: ver + 'A' };
  if (model === 0) return { target: 'm8-headless', label: 'M8 Headless', version: ver + 'A' };
  return { target: null, label: `M8 (model ${model})`, version: ver };
}

/** A transport is { write(Uint8Array): Promise, read(): Promise<Uint8Array|null>, close(): Promise }. */
export async function webSerialTransport(port) {
  await port.open({ baudRate: 115200, bufferSize: 65536 });
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  return {
    port,
    write: (b) => writer.write(b),
    read: async () => { const r = await reader.read(); return r.done ? null : r.value; },
    close: async () => {
      try { await reader.cancel(); } catch { /* gone already */ }
      reader.releaseLock(); writer.releaseLock();
      try { await port.close(); } catch { /* unplugged */ }
    },
  };
}

export class M8 {
  constructor(transport) {
    this.t = transport;
    this.seq = 0;
    this.slip = new Slip();
    this.waiting = null;           // { cmd, seq, resolve, reject, timer }
    this.queue = Promise.resolve();
    this.closed = false;
    this.onclose = null;
    this.pump();
  }

  static async choose() {
    if (!('serial' in navigator)) throw new Error('This browser has no Web Serial: use Chrome, Edge or Opera on a computer.');
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: TEENSY_VID }] });
    return new M8(await webSerialTransport(port));
  }

  async pump() {
    try {
      for (;;) {
        const chunk = await this.t.read();
        if (!chunk) break;
        for (const f of this.slip.push(chunk)) {
          const w = this.waiting;
          if (w && f.length >= 4 && f[0] === REPLY && f[1] === w.cmd && f[2] === w.seq) {
            this.waiting = null; clearTimeout(w.timer);
            if (f[3]) w.reject(new UnitError(STATUS[f[3]] || `status ${f[3]}`, f[3]));
            else w.resolve(f.subarray(4));
          }
        }
      }
    } catch (e) { /* the port went away */ }
    this.closed = true;
    if (this.waiting) { this.waiting.reject(new UnitError('the M8 was disconnected')); this.waiting = null; }
    if (this.onclose) this.onclose();
  }

  /** One request at a time: each waits for the one before. */
  request(cmd, payload = new Uint8Array(), timeout = 3000) {
    const run = () => new Promise((resolve, reject) => {
      if (this.closed) { reject(new UnitError('the M8 is not connected')); return; }
      this.seq = (this.seq + 1) & 0xFF;
      const timer = setTimeout(() => {
        if (this.waiting && this.waiting.timer === timer) {
          this.waiting = null;
          reject(new UnitError('no reply: is dirtyloader core on this M8, and m8c (or another page) closed?'));
        }
      }, timeout);
      this.waiting = { cmd, seq: this.seq, resolve, reject, timer };
      this.t.write(requestBytes(cmd, this.seq, payload)).catch(reject);
    });
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => {});
    return p;
  }

  async close() { this.closed = true; await this.t.close(); }

  // -- the unit -------------------------------------------------------------------------------------------------
  async info() {
    const text = dec.decode(await this.request(CMD.INFO));
    const line = (k) => (text.split('\n').find(l => l.startsWith(k + ' ')) || '').slice(k.length + 1);
    const fw = parseInt(line('firmware'), 16);
    return { text, core: (text.match(/core ([\d.]+)/) || [])[1] || '?', fw, ...describeFw(fw),
      sd: line('sd') === 'mounted', area: parseInt(line('mods area'), 10) || 0,
      loaded: line('loaded').split(' ').filter(Boolean) };
  }
  async list(dir = 'mods') { return dec.decode(await this.request(CMD.LIST, enc.encode(dir))).split('\n').filter(Boolean); }
  async get(name) {
    const parts = [];
    let got = 0;
    for (;;) {
      const part = await this.getRange(name, got, 512);
      parts.push(part); got += part.length;
      if (part.length < 512) return cat(...parts);
    }
  }
  async getRange(name, off, max) {
    const n = enc.encode(name);
    return this.request(CMD.GET, cat(Uint8Array.from(le32(off)), Uint8Array.of(max & 0xFF, max >> 8, n.length), n));
  }
  /** A mod file's header without reading all of it (3 requests): its id, version, firmware and hooks (core checks
   *  the rest when it loads it). */
  async head(name) {
    const h = await this.getRange(name, 0, 56);
    const magic = dec.decode(h.subarray(0, 4));
    const hs = magic === 'DLX2' ? 24 : magic === 'DLX1' ? 20 : 0;
    if (!hs || h.length < hs + 28 || u32(h, hs) !== 0x314D4C44) throw new Error('not an .m8x');
    const str = async (p) => { const b = await this.getRange(name, hs + p, 64); let e = 0; while (e < b.length && b[e]) e++; return dec.decode(b.subarray(0, e)); };
    return { id: await str(u32(h, hs + 8)), version: await str(u32(h, hs + 12)), fw: u32(h, hs + 24),
      hooks: h[hs + 6] | h[hs + 7] << 8, image: u32(h, 4) };
  }
  async put(name, data, onProgress) {
    const n = enc.encode(name);
    for (let off = 0; off < Math.max(data.length, 1); off += CHUNK) {
      const chunk = data.subarray(off, off + CHUNK);
      const r = await this.request(CMD.PUT, cat(Uint8Array.from(le32(off)), Uint8Array.of(n.length), n, chunk), 5000);
      if (u32(r, 0) !== chunk.length) throw new UnitError(`${name}: the card took ${u32(r, 0)} of ${chunk.length} bytes`);
      if (onProgress) onProgress(Math.min(1, (off + chunk.length) / data.length));
    }
  }
  async check(name) { const r = await this.request(CMD.CHECK, enc.encode(name), 8000); return { size: u32(r, 0), crc: u32(r, 4) }; }
  mkdirs() { return this.request(CMD.MKDIRS); }
  remove(name) { return this.request(CMD.DELETE, enc.encode(name)); }
  restart() { return this.request(CMD.RESTART); }

  async active() {
    try {
      return dec.decode(await this.get(ACTIVE)).split(/\r?\n/).map(l => l.trim()).filter(l => l && !l.startsWith('#'));
    } catch (e) { if (e instanceof UnitError) return []; throw e; }
  }
  async setActive(names) {
    const text = '# one .m8x per line, from /dirtyloader/mods/; core loads them at boot\n' + names.map(n => n + '\n').join('');
    await this.put(ACTIVE, enc.encode(text));
    await this.put('guard', enc.encode('GOOD'));            // a new set gets a fresh try
  }
  async status() { try { return dec.decode(await this.get('status.txt')); } catch { return ''; } }

  /** The mods on the card: name, its header (id, version, title) read from the file's start, and whether it's on. */
  async mods() {
    const names = (await this.list('mods')).filter(n => n.toLowerCase().endsWith('.m8x'));
    const act = await this.active();
    const out = [];
    for (const name of names) {
      let head = null;
      try { head = await this.head('mods/' + name); } catch { head = null; }
      out.push({ name, on: act.includes(name), head });
    }
    return out;
  }

  /** Upload, verify by crc32, and switch on for the next boot (python: Unit.add). */
  async install(name, data, { on = true, onProgress } = {}) {
    parseM8x(data);                                         // refuse anything core would refuse
    await this.mkdirs();
    await this.put('mods/' + name, data, onProgress);
    const { size, crc } = await this.check('mods/' + name);
    if (size !== data.length || crc !== crc32(data)) throw new UnitError(`${name}: the copy on the card does not match (verify failed)`);
    const act = await this.active();
    if (on && !act.includes(name)) await this.setActive([...act, name]);
    if (!on && act.includes(name)) await this.setActive(act.filter(n => n !== name));
  }
  async setOn(name, on) {
    const act = (await this.active()).filter(n => n !== name);
    await this.setActive(on ? [...act, name] : act);
  }
  async uninstall(name) {
    const act = await this.active();
    if (act.includes(name)) await this.setActive(act.filter(n => n !== name));
    await this.remove('mods/' + name);
  }

  // -- the theme (core 0.7: THEME) --------------------------------------------------------------------------------
  /** The 13 colours (39 bytes, .m8t order) and the name the THEME page shows. */
  async theme() {
    const r = await this.request(CMD.THEME);
    let e = 39; while (e < 52 && r[e]) e++;
    return { colors: r.slice(0, 39), name: dec.decode(r.subarray(39, e)) };
  }
  /** Repaint the M8 in these colours now; with save, also keep them across power-off (EEPROM), with the name. */
  /** core's extended theme (THEMEX): flags (1 track colours, 2 screen titles, 4 titles follow the track) and 15
   *  colours (tracks 1-8, then the titles of SONG, CHAIN, PHRASE, INSTRUMENT, TABLE, MIXER and the other screens). */
  async themeExt() {
    const r = await this.request(CMD.THEMEX);
    return { flags: r[0], rgb: r.slice(1, 46) };
  }
  /** Set them now (every view repaints); with save, also in /dirtyloader/theme.ext for the next boot. */
  setThemeExt(flags, rgb, { save = false } = {}) {
    return this.request(CMD.THEMEX, cat(Uint8Array.of(flags & 7), rgb.subarray(0, 45), save ? Uint8Array.of(1) : new Uint8Array()));
  }
  setTheme(colors, { save = false, name = '' } = {}) {
    const tail = save || name ? cat(Uint8Array.of(save ? 1 : 0), enc.encode(name.toUpperCase().slice(0, 12))) : new Uint8Array();
    return this.request(CMD.THEME, cat(colors, tail));
  }
}
