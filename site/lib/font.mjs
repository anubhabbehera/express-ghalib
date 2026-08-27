/**
 * font.mjs — reads a TrueType `cmap` so the build can tell which characters
 * PixelOperator can actually draw. Anything outside that set falls back to a
 * system font in the browser, which breaks the illusion instantly.
 */
import { promises as fs } from 'node:fs';

/** Returns a Set of codepoints the font has a glyph for. */
export async function fontCoverage(fontPath) {
  const buf = await fs.readFile(fontPath);

  const numTables = buf.readUInt16BE(4);
  let cmapOffset = null;
  for (let i = 0; i < numTables; i++) {
    const rec = 12 + i * 16;
    if (buf.toString('latin1', rec, rec + 4) === 'cmap') cmapOffset = buf.readUInt32BE(rec + 8);
  }
  if (cmapOffset === null) throw new Error(`${fontPath}: no cmap table`);

  // Prefer a Windows Unicode BMP (3,1) or full-repertoire (3,10) subtable.
  const numSub = buf.readUInt16BE(cmapOffset + 2);
  let subtable = null;
  for (let i = 0; i < numSub; i++) {
    const rec = cmapOffset + 4 + i * 8;
    const platform = buf.readUInt16BE(rec);
    const encoding = buf.readUInt16BE(rec + 2);
    if (platform === 3 && (encoding === 1 || encoding === 10)) subtable = cmapOffset + buf.readUInt32BE(rec + 4);
  }
  if (subtable === null) throw new Error(`${fontPath}: no Unicode cmap subtable`);

  const format = buf.readUInt16BE(subtable);
  if (format !== 4) throw new Error(`${fontPath}: unsupported cmap format ${format}`);

  const segCountX2 = buf.readUInt16BE(subtable + 6);
  const segCount = segCountX2 / 2;
  const endOff = subtable + 14;
  const startOff = endOff + segCountX2 + 2;
  const deltaOff = startOff + segCountX2;
  const rangeOff = deltaOff + segCountX2;

  const covered = new Set();
  for (let i = 0; i < segCount; i++) {
    const end = buf.readUInt16BE(endOff + i * 2);
    const start = buf.readUInt16BE(startOff + i * 2);
    const delta = buf.readInt16BE(deltaOff + i * 2);
    const rangeOffset = buf.readUInt16BE(rangeOff + i * 2);
    for (let cp = start; cp <= end && cp !== 0xffff; cp++) {
      let glyph;
      if (rangeOffset === 0) {
        glyph = (cp + delta) & 0xffff;
      } else {
        const idx = rangeOff + i * 2 + rangeOffset + (cp - start) * 2;
        if (idx + 1 >= buf.length) continue;
        glyph = buf.readUInt16BE(idx);
        if (glyph) glyph = (glyph + delta) & 0xffff;
      }
      if (glyph) covered.add(cp);
    }
  }
  return covered;
}
