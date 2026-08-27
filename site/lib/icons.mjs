/**
 * icons.mjs — decodes the launcher icons out of src/img_icons.c and encodes
 * them as PNGs, so the website shows the exact bitmaps the device draws.
 *
 * The generated file holds LVGL `LV_IMG_CF_INDEXED_1BIT` images: an 8-byte
 * two-colour palette (BGRA, BGRA) followed by a 1-bit bitmap whose rows are
 * padded to a byte boundary. Icons are 32x32, so 4 bytes per row.
 */
import { promises as fs } from 'node:fs';
import zlib from 'node:zlib';

const CRC_TABLE = (() => {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c;
  }
  return table;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (const byte of buf) c = CRC_TABLE[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const out = Buffer.alloc(data.length + 12);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, 'latin1');
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}

/**
 * Encodes a greyscale + alpha bitmap as a PNG: two bytes per pixel, value then
 * alpha. The icons' background must be transparent, not white, so a tile shows
 * the panel ground behind the art the way the device does.
 */
function encodePng(width, height, pixels) {
  const rowBytes = width * 2;
  const raw = Buffer.alloc((rowBytes + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (rowBytes + 1)] = 0; // filter: none
    pixels.copy(raw, y * (rowBytes + 1) + 1, y * rowBytes, (y + 1) * rowBytes);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 4; // colour type: greyscale + alpha
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/**
 * Parses img_icons.c.
 * Returns Map<appName, {normal: Buffer, inverted: Buffer}> keyed by the
 * lower-case app name, e.g. "notes".
 */
export async function decodeIcons(sourcePath) {
  const source = await fs.readFile(sourcePath, 'utf8');

  const maps = new Map();
  const mapRe = /static const uint8_t (\w+)_map\[\] = \{([\s\S]*?)\};/g;
  for (let m; (m = mapRe.exec(source)) !== null; ) {
    const bytes = m[2].match(/0x[0-9a-fA-F]{2}/g) ?? [];
    maps.set(m[1], Buffer.from(bytes.map((b) => parseInt(b, 16))));
  }

  const icons = new Map();
  const dscRe = /const lv_img_dsc_t (\w+) = \{([\s\S]*?)\};/g;
  for (let m; (m = dscRe.exec(source)) !== null; ) {
    const name = m[1];
    const body = m[2];
    const w = Number(/\.header\.w = (\d+)/.exec(body)?.[1]);
    const h = Number(/\.header\.h = (\d+)/.exec(body)?.[1]);
    const data = maps.get(name);
    if (!data || !w || !h) continue;
    if (!/INDEXED_1BIT/.test(body)) throw new Error(`${name}: unsupported colour format`);

    // Palette: two BGRA entries. Index 0 is drawn with the first colour.
    const palette = [data[0], data[4]]; // blue channel is enough for 1-bit art
    const bitmap = data.subarray(8);
    const stride = Math.ceil(w / 8);
    // Index 0 is the background the icon was drawn on; it becomes transparent.
    const pixels = Buffer.alloc(w * h * 2);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const bit = (bitmap[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
        const at = (y * w + x) * 2;
        pixels[at] = palette[bit];
        pixels[at + 1] = bit ? 255 : 0;
      }
    }

    const base = name.replace(/^img_icon_/, '');
    const inverted = base.endsWith('_inv');
    const key = inverted ? base.slice(0, -4) : base;
    const entry = icons.get(key) ?? {};
    entry[inverted ? 'inverted' : 'normal'] = encodePng(w, h, pixels);
    icons.set(key, entry);
  }

  for (const [key, entry] of icons) {
    if (!entry.normal || !entry.inverted) throw new Error(`icon ${key}: missing a variant`);
  }
  return icons;
}
