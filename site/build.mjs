/**
 * build.mjs — renders the project's markdown into a static site that looks
 * like the device's reflective panel.
 *
 *   npm run site:build      -> _site/
 *   npm run site:serve      -> http://localhost:4200
 *
 * Everything the page needs is generated here: the launcher icons are decoded
 * straight out of src/img_icons.c (the same 1-bit bitmaps the firmware ships),
 * and the fonts are the same PixelOperator TTFs the LVGL fonts were built from.
 * Nothing is fetched at page load.
 */
import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { marked } from 'marked';
import { decodeIcons } from './lib/icons.mjs';
import { fontCoverage } from './lib/font.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, '_site');
// Built into a staging directory and swapped in at the end, so `site:serve
// --watch` never serves (and `site:check` never inspects) a half-written tree.
const STAGE = path.join(ROOT, '_site.tmp');

// --strict turns the glyph-coverage warning into a build failure. CI uses it so
// a stray character can never reach the published site in a fallback font.
const STRICT = process.argv.includes('--strict');

const SITE = {
  title: 'express-ghalib',
  tagline: 'A pocket, keyboard-driven PDA firmware for the Waveshare ESP32-S3-RLCD-4.2.',
  repo: 'https://github.com/anubhabbehera/express-ghalib',
};

// Panel geometry, straight from src/st7305.h. The device chrome is laid out in
// these units and scaled by an integer factor so the pixels stay square.
const PANEL = { w: 400, h: 300, bar: 34 };

// Pages: [source markdown, output path, nav label].
const PAGES = [
  { src: 'README.md', out: 'index.html', label: 'Home', home: true },
  { src: 'docs/README.md', out: 'docs/index.html', label: 'Docs' },
  { src: 'docs/architecture.md', out: 'docs/architecture.html', label: 'Architecture' },
  { src: 'docs/display-pipeline.md', out: 'docs/display-pipeline.html', label: 'Display' },
  { src: 'docs/performance.md', out: 'docs/performance.html', label: 'Performance' },
  { src: 'docs/hardware.md', out: 'docs/hardware.html', label: 'Hardware' },
  { src: 'docs/build-flash-debug.md', out: 'docs/build-flash-debug.html', label: 'Build' },
];

// PixelOperator covers Latin-1 plus common punctuation, but not everything the
// docs use. Anything the font cannot draw would silently fall back to a
// non-pixel system font, so it is rewritten to an ASCII equivalent instead.
const SUBSTITUTIONS = [
  [/‑/g, '-'],       // non-breaking hyphen
  [/→/g, '->'],
  [/←/g, '<-'],
  [/⇒/g, '=>'],
  [/✓|✔/g, '[x]'],
  [/✗|✘/g, '[ ]'],
  [/ /g, ' '],
  [/≤/g, '<='],
  [/≥/g, '>='],
  [/≈/g, '~'],
  [/≠/g, '!='],
  [/Ω/g, 'ohm'],
  [/►|▶/g, '>'],
  [/⚠/g, '!'],
  [/️/g, ''],   // emoji variation selector, left behind by "⚠️"
];

const FONTS = ['PixelOperator.ttf', 'PixelOperator-Bold.ttf', 'PixelOperatorMono.ttf', 'PixelOperatorMono-Bold.ttf'];

// --- helpers ---------------------------------------------------------------

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

const stripTags = (s) => s.replace(/<[^>]+>/g, '');

/**
 * GitHub's heading-anchor algorithm, so links written for the markdown on
 * GitHub (`#direct_mode-is-on-deliberately`, `#8-power--deep-sleep`) resolve to
 * the same ids here: strip punctuation but keep underscores, then turn each
 * remaining whitespace character into its own hyphen.
 */
function slug(text) {
  return stripTags(text)
    .toLowerCase()
    .replace(/&[a-z]+;/g, '')
    .replace(/[^a-z0-9\s_-]/g, '')
    .trim()
    .replace(/\s/g, '-');
}

function substitute(text) {
  return SUBSTITUTIONS.reduce((acc, [re, to]) => acc.replace(re, to), text);
}

/** Depth of a page below the site root, e.g. docs/architecture.html -> 1. */
const depthOf = (out) => out.split('/').length - 1;

const prefixFor = (out) => '../'.repeat(depthOf(out));

/**
 * Rewrites an in-repo markdown link to its built page. Links that point at
 * source files (src/, tools/, platformio.ini) go to GitHub instead — those
 * files are not part of the site.
 */
function rewriteHref(href, page) {
  if (/^(https?:|mailto:|#|\/)/.test(href)) return href;
  const [rawPath, hash = ''] = href.split('#');
  if (!rawPath) return href;

  const fromDir = path.posix.dirname(page.src);
  const target = path.posix.normalize(path.posix.join(fromDir === '.' ? '' : fromDir, rawPath));
  const match = PAGES.find((p) => p.src === target);
  if (match) {
    const rel = path.posix.relative(path.posix.dirname(page.out), match.out) || path.posix.basename(match.out);
    return rel + (hash ? `#${hash}` : '');
  }
  return `${SITE.repo}/blob/main/${target}${hash ? `#${hash}` : ''}`;
}

// --- markdown --------------------------------------------------------------

marked.setOptions({ gfm: true, breaks: false, mangle: false, headerIds: false });

/**
 * Renders one markdown file. Returns the body HTML plus the heading outline
 * used for the sidebar index.
 */
function renderMarkdown(md, page) {
  let html = marked.parse(substitute(md));
  const outline = [];
  const ids = new Set();

  // Heading ids + a permalink target for the sidebar.
  html = html.replace(/<h([1-6])>([\s\S]*?)<\/h\1>/g, (_m, level, inner) => {
    const id = slug(inner);
    const depth = Number(level);
    ids.add(id);
    if (depth === 2 || depth === 3) outline.push({ id, depth, text: stripTags(inner) });
    return `<h${level} id="${id}"><a class="anchor" href="#${id}" aria-label="Link to this section">#</a>${inner}</h${level}>`;
  });

  html = html.replace(/href="([^"]*)"/g, (_m, href) => `href="${esc(rewriteHref(href, page))}"`);

  // Tables are the one thing that will not fit a narrow panel; give each its
  // own scroll box so the page body never scrolls sideways.
  html = html.replace(/<table>/g, '<div class="scrollbox"><table>').replace(/<\/table>/g, '</table></div>');

  // The first h1 becomes the page header, not body content.
  let title = SITE.title;
  html = html.replace(/<h1[^>]*>([\s\S]*?)<\/h1>\n?/, (_m, inner) => {
    title = stripTags(inner);
    return '';
  });

  return { html, outline, title, ids };
}

/**
 * Pulls the app roster out of README section 5, in the order the launcher
 * shows them. A tile links to its app's section on this page, so the launcher
 * and the README can never drift apart.
 */
function extractApps(readme) {
  const section = readme.split(/^## 5\. Apps$/m)[1] ?? '';
  const body = section.split(/^## /m)[0];
  return [...body.matchAll(/^### (.+)$/gm)].map((m) => m[1].trim());
}

// --- page shell ------------------------------------------------------------

function navHtml(page) {
  const prefix = prefixFor(page.out);
  return PAGES.map((p) => {
    const href = prefix + p.out.replace(/index\.html$/, '');
    const current = p.out === page.out;
    return `<a class="nav-item${current ? ' is-current' : ''}" href="${href || './'}"${current ? ' aria-current="page"' : ''}>${esc(p.label)}</a>`;
  }).join('\n        ');
}

function outlineHtml(outline) {
  if (outline.length < 2) return '';
  const items = outline
    .map((h) => `<a class="toc-item toc-h${h.depth}" href="#${h.id}">${esc(h.text)}</a>`)
    .join('\n          ');
  return `
      <nav class="toc" aria-label="On this page">
        <div class="toc-title">Contents</div>
        <div class="toc-list">
          ${items}
        </div>
      </nav>`;
}

/**
 * The launcher demo: a real-geometry 400x300 panel with the firmware's status
 * bar and 3x4 tile grid, driven by the arrow keys like the device is.
 */
function deviceHtml(apps, icons, lead, ids) {
  const tiles = apps
    .filter((name) => icons.has(name.toLowerCase()))
    .map((name) => {
      const key = name.toLowerCase();
      const anchor = ids.has(slug(name)) ? slug(name) : null;
      const art = `<img class="tile-icon" src="assets/icons/${key}.png" alt="" width="32" height="32">
              <img class="tile-icon tile-icon-inv" src="assets/icons/${key}-inv.png" alt="" width="32" height="32">
              <span class="tile-label">${esc(name)}</span>`;
      // No section on the page means the tile does nothing, rather than
      // pretending to lead somewhere.
      return anchor
        ? `<a class="tile" href="#${anchor}" data-app="${esc(name)}" tabindex="-1">
              ${art}
            </a>`
        : `<span class="tile is-inert" data-app="${esc(name)}" aria-hidden="true">
              ${art}
            </span>`;
    })
    .join('\n            ');

  return `
    <section class="hero">
      <div class="device" id="device" tabindex="0" role="application"
           aria-label="Launcher demo. Arrow keys move between apps, Enter jumps to that app's section."
           style="--panel-w:${PANEL.w};--panel-h:${PANEL.h}">
        <div class="panel">
          <div class="panel-bar">
            <span class="bar-wordmark">express-ghalib</span>
            <span class="bar-clock" data-clock>--:--</span>
            <span class="bar-right"><span class="bar-batt" data-batt>87%</span><span class="bar-glyph" title="Wi-Fi">wifi</span><span class="bar-glyph" title="Bluetooth">ble</span></span>
          </div>
          <div class="panel-body">
            <div class="grid" id="tile-grid">
            ${tiles}
            </div>
          </div>
        </div>
      </div>
      <div class="hero-side">
        <p class="hero-tagline">${lead}</p>
        <p class="hero-note">That is the real launcher: the icons are the 1-bit bitmaps
          from <code>src/img_icons.c</code>, the type is PixelOperator at the same
          16&nbsp;px the firmware uses, and the panel is 400&times;300 like the ST7305.</p>
        <p class="hero-note">Click a tile, or click the screen and use the
          <b>arrow keys</b> and <b>Enter</b>, to jump to that app's section below.</p>
        <div class="hero-links">
          <a class="btn" href="docs/">Documentation</a>
          <a class="btn" href="${SITE.repo}">Source on GitHub</a>
        </div>
      </div>
    </section>`;
}

function shell({ page, title, body, outline, hero }) {
  const prefix = prefixFor(page.out);
  const description = page.home ? SITE.tagline : `${title} — express-ghalib firmware documentation.`;
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(page.home ? SITE.title : `${title} · ${SITE.title}`)}</title>
<meta name="description" content="${esc(description)}">
<meta name="color-scheme" content="light">
<meta property="og:title" content="${esc(page.home ? SITE.title : `${title} · ${SITE.title}`)}">
<meta property="og:description" content="${esc(description)}">
<meta property="og:type" content="website">
<link rel="icon" href="${prefix}assets/icons/notes.png">
<link rel="stylesheet" href="${prefix}assets/rlcd.css">
</head>
<body data-page="${esc(page.out)}">
<a class="skip" href="#content">Skip to content</a>

<header class="statusbar">
  <div class="statusbar-inner">
    <a class="wordmark" href="${prefix}">express-ghalib</a>
    <span class="clock" data-clock>--:--</span>
    <nav class="nav" aria-label="Sections">
        ${navHtml(page)}
    </nav>
  </div>
</header>

<main id="content" class="page">
  ${hero ?? ''}
  <div class="page-inner">
    ${outlineHtml(outline)}
    <article class="prose">
      <h1 class="page-title">${esc(title)}</h1>
      ${body}
    </article>
  </div>
</main>

<footer class="hintbar">
  <div class="hintbar-inner">
    <span class="hint"><b>[Up/Dn]</b> scroll</span>
    <span class="hint"><b>[D]</b> docs</span>
    <span class="hint"><b>[H]</b> home</span>
    <span class="hint"><button class="hint-btn" type="button" data-action="text-down"><b>[-]</b></button><button class="hint-btn" type="button" data-action="text-up"><b>[+]</b></button> text size</span>
    <span class="hint"><button class="hint-btn" type="button" data-action="keys"><b>[?]</b></button> keys</span>
    <span class="hint hint-repo"><a href="${SITE.repo}">github</a></span>
  </div>
</footer>

<div class="overlay" id="keys-overlay" hidden>
  <div class="overlay-box" role="dialog" aria-modal="true" aria-label="Keyboard shortcuts">
    <div class="overlay-title">Keys</div>
    <table class="keys">
      <tr><td>Up / Dn, j / k</td><td>scroll a line</td></tr>
      <tr><td>Space / PgDn</td><td>page down</td></tr>
      <tr><td>g / G</td><td>top / bottom</td></tr>
      <tr><td>h</td><td>home</td></tr>
      <tr><td>d</td><td>documentation index</td></tr>
      <tr><td>+ / -</td><td>text size</td></tr>
      <tr><td>Esc</td><td>close this</td></tr>
    </table>
    <div class="overlay-hint">[Esc] close</div>
  </div>
</div>

<script src="${prefix}assets/site.js" defer></script>
</body>
</html>
`;
}

// --- build -----------------------------------------------------------------

async function copyDir(from, to) {
  await fs.mkdir(to, { recursive: true });
  for (const entry of await fs.readdir(from, { withFileTypes: true })) {
    const src = path.join(from, entry.name);
    const dst = path.join(to, entry.name);
    if (entry.isDirectory()) await copyDir(src, dst);
    else await fs.copyFile(src, dst);
  }
}

async function main() {
  await fs.rm(STAGE, { recursive: true, force: true });
  await fs.mkdir(STAGE, { recursive: true });

  // Assets: stylesheet, script, fonts, launcher icons.
  await fs.mkdir(path.join(STAGE, 'assets/fonts'), { recursive: true });
  await fs.mkdir(path.join(STAGE, 'assets/icons'), { recursive: true });
  await fs.copyFile(path.join(ROOT, 'site/assets/rlcd.css'), path.join(STAGE, 'assets/rlcd.css'));
  await fs.copyFile(path.join(ROOT, 'site/assets/site.js'), path.join(STAGE, 'assets/site.js'));
  for (const font of FONTS) {
    await fs.copyFile(path.join(ROOT, 'tools/fonts', font), path.join(STAGE, 'assets/fonts', font));
  }

  const icons = await decodeIcons(path.join(ROOT, 'src/img_icons.c'));
  for (const [name, variants] of icons) {
    await fs.writeFile(path.join(STAGE, 'assets/icons', `${name}.png`), variants.normal);
    await fs.writeFile(path.join(STAGE, 'assets/icons', `${name}-inv.png`), variants.inverted);
  }

  const readme = await fs.readFile(path.join(ROOT, 'README.md'), 'utf8');
  const apps = extractApps(readme);

  // Fail loudly if a page needs a glyph PixelOperator does not have — silent
  // font fallback is exactly the thing this site exists to avoid.
  const covered = await fontCoverage(path.join(ROOT, 'tools/fonts/PixelOperator.ttf'));
  const missing = new Map();

  for (const page of PAGES) {
    const md = await fs.readFile(path.join(ROOT, page.src), 'utf8');
    const { html, outline, title, ids } = renderMarkdown(md, page);
    for (const ch of substitute(md)) {
      const cp = ch.codePointAt(0);
      if (cp > 0x7e && cp !== 0x0a && !covered.has(cp)) {
        missing.set(ch, (missing.get(ch) ?? 0) + 1);
      }
    }
    // The landing page opens with the README's own first paragraph, set beside
    // the panel; leaving it in the body too would just repeat it.
    let body = html;
    let lead = SITE.tagline;
    if (page.home) {
      body = body.replace(/<p>([\s\S]*?)<\/p>\n?/, (_m, inner) => {
        lead = inner;
        return '';
      });
    }
    const hero = page.home ? deviceHtml(apps, icons, lead, ids) : null;
    const out = path.join(STAGE, page.out);
    await fs.mkdir(path.dirname(out), { recursive: true });
    await fs.writeFile(out, shell({ page, title, body, outline, hero }));
  }

  // A page that needs a glyph PixelOperator lacks must not replace a good
  // build, so this is checked before the staging directory is swapped in.
  if (missing.size) {
    const list = [...missing.entries()]
      .map(([ch, n]) => `U+${ch.codePointAt(0).toString(16).toUpperCase().padStart(4, '0')} (${n}x)`)
      .join(', ');
    const message = `characters not in PixelOperator, they would fall back to a system font: ${list}`;
    const hint = 'add a rewrite to SUBSTITUTIONS in site/build.mjs';
    if (STRICT) {
      console.error(`error: ${message}`);
      console.error(hint);
      await fs.rm(STAGE, { recursive: true, force: true });
      process.exitCode = 1;
      return;
    }
    console.warn(`warning: ${message}`);
    console.warn(hint);
  }

  // Jekyll would otherwise eat the _-prefixed paths GitHub Pages sees.
  await fs.writeFile(path.join(STAGE, '.nojekyll'), '');

  await fs.rm(OUT, { recursive: true, force: true });
  await fs.rename(STAGE, OUT);

  console.log(`built ${PAGES.length} pages, ${icons.size * 2} icons -> _site/`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
