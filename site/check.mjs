/**
 * check.mjs — verifies a built site before it is published.
 *
 *   npm run site:check      (runs after `npm run site:build -- --strict`)
 *
 * Catches the two failures that are invisible until someone opens the page:
 * a link that 404s, and a page asset that was never written.
 */
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, '_site');

const REQUIRED = [
  'index.html',
  'docs/index.html',
  'assets/rlcd.css',
  'assets/site.js',
  'assets/fonts/PixelOperator.ttf',
  'assets/icons/notes.png',
  '.nojekyll',
];

async function walk(dir) {
  const found = [];
  for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) found.push(...(await walk(full)));
    else found.push(full);
  }
  return found;
}

const exists = (p) => fs.access(p).then(() => true, () => false);

async function main() {
  if (!(await exists(OUT))) {
    console.error('_site/ does not exist — run `npm run site:build` first');
    process.exit(1);
  }

  const problems = [];

  for (const rel of REQUIRED) {
    if (!(await exists(path.join(OUT, rel)))) problems.push(`missing required file: ${rel}`);
  }

  const files = await walk(OUT);
  const pages = files.filter((f) => f.endsWith('.html'));
  let internal = 0;
  let external = 0;
  const anchorsByFile = new Map();

  for (const page of pages) {
    const html = await fs.readFile(page, 'utf8');
    anchorsByFile.set(page, new Set([...html.matchAll(/ id="([^"]+)"/g)].map((m) => m[1])));
  }

  for (const page of pages) {
    const html = await fs.readFile(page, 'utf8');
    const rel = path.relative(OUT, page);

    for (const match of html.matchAll(/(?:href|src)="([^"]+)"/g)) {
      const url = match[1];
      if (/^(https?:|mailto:)/.test(url)) {
        external++;
        continue;
      }
      internal++;

      const [target, hash] = url.split('#');
      if (!target) {
        // Same-page anchor.
        if (hash && !anchorsByFile.get(page).has(hash)) problems.push(`${rel}: no such anchor #${hash}`);
        continue;
      }

      let resolved = path.resolve(path.dirname(page), target);
      if (target.endsWith('/') || !path.extname(resolved)) resolved = path.join(resolved, 'index.html');
      if (!resolved.startsWith(OUT)) {
        problems.push(`${rel}: link escapes the site root: ${url}`);
        continue;
      }
      if (!(await exists(resolved))) {
        problems.push(`${rel}: broken link ${url}`);
        continue;
      }
      if (hash && resolved.endsWith('.html') && !anchorsByFile.get(resolved)?.has(hash)) {
        problems.push(`${rel}: ${url} points at a missing anchor`);
      }
    }
  }

  console.log(`checked ${pages.length} pages, ${internal} internal links, ${external} external links`);
  if (problems.length) {
    problems.forEach((p) => console.error(`  ${p}`));
    console.error(`${problems.length} problem(s)`);
    process.exit(1);
  }
  console.log('site ok');
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
