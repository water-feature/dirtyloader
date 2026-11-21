// The mod shop's JavaScript (site/shop): the protocol against a pretend core, the catalog's files, the theme file.
//   node tests/test_shop.mjs
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';
import { M8, parseM8x, crc32, requestBytes, Slip, webSerialTransport, describeFw } from '../site/shop/m8.js';
import { parseM8t, encodeM8t, tokens, contrast, PRESETS, EXT_PRESETS, EXT_SLOTS } from '../site/shop/theme.js';
import { FakeCore, fakePort } from '../site/shop/test/fake_m8.js';

const SHOP = new URL('../site/shop/', import.meta.url);
let n = 0;
const test = async (name, fn) => { await fn(); n++; console.log('ok', name); };

await test('crc32 is zlib\'s', () => assert.equal(crc32(new TextEncoder().encode('123456789')), 0xCBF43926));
await test('a request frame', () => assert.deepEqual([...requestBytes(1, 7)], [0xD1, 1, 7, 0, 0, 8]));
await test('SLIP unescapes across chunks', () => {
  const s = new Slip();
  assert.deepEqual(s.push([0xF1, 0xDB]), []);
  assert.deepEqual(s.push([0xDC, 0xC0]).map(f => [...f]), [[0xF1, 0xC0]]);
});

const catalog = JSON.parse(readFileSync(new URL('catalog.json', SHOP)));
await test('every catalog file is the one it lists, for the right firmware', () => {
  const fws = { 'm8-model02': 0x02060602, 'm8-model02-664': 0x02060604, 'm8-headless': 0x00060604, 'm8-model01': 0x01060604 };
  for (const m of catalog.mods) for (const [t, b] of Object.entries(m.builds)) {
    const data = readFileSync(new URL(b.file, SHOP));
    assert.equal(createHash('sha256').update(data).digest('hex'), b.sha256, b.file);
    const h = parseM8x(new Uint8Array(data));
    assert.equal(h.id, m.id); assert.equal(h.version, b.version); assert.ok(h.fw === fws[t] || h.fw === 0, b.file);   // 0: any firmware
  }
});
await test('each firmware id names its shop target', () => {
  assert.equal(describeFw(0x02060602).target, 'm8-model02'); assert.equal(describeFw(0x02060604).target, 'm8-model02-664');
  assert.equal(describeFw(0x00060604).target, 'm8-headless');
  assert.equal(describeFw(0x01060604).target, 'm8-model01');
  assert.equal(describeFw(0x01060603).target, null);
  for (const t of ['m8-model02', 'm8-model02-664', 'm8-headless', 'm8-model01']) assert.ok(catalog.targets[t], t);
});
await test('parseM8x refuses a damaged file', () => {
  const d = new Uint8Array(readFileSync(new URL(catalog.mods.find(m => m.id === 'chaos-control').builds['m8-model02'].file, SHOP)));
  d[100] ^= 0xFF;
  assert.throws(() => parseM8x(d), /checksum/);
});

const port = fakePort(new FakeCore({ loaded: ['perf-mon'], files: { '/Themes/A.m8t': encodeM8t(PRESETS[1].colors) } }));
const m8 = new M8(await webSerialTransport(port));
await test('info', async () => {
  const i = await m8.info();
  assert.equal(i.target, 'm8-model02'); assert.equal(i.core, '0.7'); assert.deepEqual(i.loaded, ['perf-mon']);
});
await test('install: upload, verify, switch on', async () => {
  const b = catalog.mods.find(m => m.id === 'chaos-control').builds['m8-model02'];
  const data = new Uint8Array(readFileSync(new URL(b.file, SHOP)));
  let last = 0;
  await m8.install('chaos-control.m8x', data, { onProgress: p => { last = p; } });
  assert.equal(last, 1);
  assert.deepEqual(port.core.files.get('/dirtyloader/mods/chaos-control.m8x'), data);
  assert.deepEqual(await m8.active(), ['chaos-control.m8x']);
  const mods = await m8.mods();
  assert.equal(mods[0].head.id, 'chaos-control'); assert.equal(mods[0].on, true);
});
await test('switch off, then delete', async () => {
  await m8.setOn('chaos-control.m8x', false);
  assert.deepEqual(await m8.active(), []);
  await m8.uninstall('chaos-control.m8x');
  assert.deepEqual(await m8.list('mods'), []);
});
await test('theme: read, preview, save with a name', async () => {
  const t = await m8.theme();
  assert.equal(t.colors.length, 39);
  await m8.setTheme(PRESETS[2].colors);
  assert.equal(port.core.saved, 0);
  await m8.setTheme(PRESETS[2].colors, { save: true, name: 'nightshift' });
  assert.equal(port.core.saved, 1);
  const u = await m8.theme();
  assert.deepEqual([...u.colors], [...PRESETS[2].colors]); assert.equal(u.name, 'NIGHTSHIFT');
});
await test('extended theme: read, set live, save to the card', async () => {
  const x = await m8.themeExt();
  assert.equal(x.flags, 0); assert.equal(x.rgb.length, 45);
  const rgb = Uint8Array.from({ length: 45 }, (_, i) => i * 5);
  await m8.setThemeExt(3, rgb);
  assert.equal(port.core.xsaved, 0);
  await m8.setThemeExt(7, rgb, { save: true });
  assert.equal(port.core.xsaved, 1);
  const y = await m8.themeExt();
  assert.equal(y.flags, 7); assert.deepEqual([...y.rgb], [...rgb]);
  const f = port.core.files.get('/dirtyloader/theme.ext');
  assert.equal(new TextDecoder().decode(f.subarray(0, 4)), 'DLXT'); assert.equal(f.length, 53);
});
await test('every extended preset has 15 colours', () => {
  for (const [k, v] of Object.entries(EXT_PRESETS)) assert.equal(v.length, 90, k);
  assert.equal(EXT_SLOTS.length, 15);
});
await test('the card\'s themes read by absolute path', async () => {
  assert.deepEqual(await m8.list('/Themes'), ['A.m8t']);
  assert.deepEqual([...parseM8t(await m8.get('/Themes/A.m8t'))], [...PRESETS[1].colors]);
});
await test('.m8t round trip, 53 bytes', () => {
  const f = encodeM8t(PRESETS[3].colors);
  assert.equal(f.length, 53);
  assert.deepEqual([...parseM8t(f)], [...PRESETS[3].colors]);
});
await test('the page stays readable in every preset', () => {
  const rgb = (s) => s.match(/\d+/g).map(Number);
  for (const p of PRESETS) {
    const t = tokens(p.colors);
    assert.ok(contrast(rgb(t['--text']), rgb(t['--bg'])) >= 4.5, p.name + ' text');
    assert.ok(contrast(rgb(t['--on-cursor']), rgb(t['--cursor'])) >= 3, p.name + ' button');
  }
});
await m8.close();
console.log(`${n} passed`);
