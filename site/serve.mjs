/**
 * serve.mjs — a zero-dependency static server for the built site, so the look
 * can be checked locally exactly as GitHub Pages will serve it.
 *
 *   npm run site:build && npm run site:serve
 *   -> http://localhost:4200
 *
 * Pass --port N to use a different port, --watch to rebuild on source changes.
 */
import http from 'node:http';
import { promises as fs } from 'node:fs';
import fsSync from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, '_site');

const args = process.argv.slice(2);
const portArg = args.indexOf('--port');
const PORT = portArg !== -1 ? Number(args[portArg + 1]) : 4200;
const WATCH = args.includes('--watch');

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.png': 'image/png',
  '.ttf': 'font/ttf',
  '.svg': 'image/svg+xml',
  '.json': 'application/json',
};

function rebuild(reason) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [path.join(ROOT, 'site/build.mjs')], { stdio: 'inherit' });
    child.on('exit', (code) => {
      if (code !== 0) console.error(`build failed (${reason})`);
      resolve(code);
    });
  });
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://localhost:${PORT}`);
    let filePath = path.join(OUT, decodeURIComponent(url.pathname));

    // Never serve outside the build directory.
    if (!filePath.startsWith(OUT)) {
      res.writeHead(403).end('forbidden');
      return;
    }

    let stat = await fs.stat(filePath).catch(() => null);
    if (stat?.isDirectory()) {
      filePath = path.join(filePath, 'index.html');
      stat = await fs.stat(filePath).catch(() => null);
    }
    if (!stat) {
      res.writeHead(404, { 'content-type': 'text/plain' }).end('404');
      return;
    }

    const body = await fs.readFile(filePath);
    res.writeHead(200, {
      'content-type': TYPES[path.extname(filePath)] ?? 'application/octet-stream',
      'cache-control': 'no-store',
    }).end(body);
  } catch (err) {
    res.writeHead(500, { 'content-type': 'text/plain' }).end(String(err));
  }
});

if (!fsSync.existsSync(OUT)) await rebuild('initial');

if (WATCH) {
  let pending = null;
  for (const dir of ['site', 'docs']) {
    fsSync.watch(path.join(ROOT, dir), { recursive: true }, () => {
      clearTimeout(pending);
      pending = setTimeout(() => rebuild('watch'), 120);
    });
  }
  fsSync.watch(path.join(ROOT, 'README.md'), () => {
    clearTimeout(pending);
    pending = setTimeout(() => rebuild('watch'), 120);
  });
  console.log('watching site/, docs/, README.md');
}

server.listen(PORT, () => {
  console.log(`serving _site/ at http://localhost:${PORT}`);
});
