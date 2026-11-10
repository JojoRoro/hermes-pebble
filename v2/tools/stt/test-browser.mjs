// Runs the built speech engine in headless Chromium, set up like the Pebble Android app's
// PebbleKit JS WebView: a file:// page with the app's Content-Security-Policy, no workers, and the
// engine arriving as a string that is evalled.
//
//   node test-browser.mjs <cache-dir> <model-id> <audio-file> [--throttle N]
//
// <cache-dir> holds model files as <repo>/<path> (see fetch-models.mjs). Needs ffmpeg and a
// Chromium (CHROMIUM_PATH, or Playwright's download).

import { chromium } from 'playwright-core';
import { execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { createReadStream, existsSync, mkdtempSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const throttleAt = args.indexOf('--throttle');
const throttle = throttleAt >= 0 ? Number(args.splice(throttleAt, 2)[1]) : 1;
const [cacheDir, modelId, audioFile] = args;
if (!cacheDir || !modelId || !audioFile) {
  console.error('usage: node test-browser.mjs <cache-dir> <model-id> <audio-file> [--throttle N]');
  process.exit(2);
}

const engine = JSON.parse(readFileSync(join(here, '../../src/pkjs/stt_engine.json'), 'utf8'));
const catalog = JSON.parse(readFileSync(join(here, '../../src/pkjs/stt_catalog.json'), 'utf8'));
const model = catalog.models.find((m) => m.id === modelId);
if (!model) throw new Error('unknown model ' + modelId);

const work = mkdtempSync(join(tmpdir(), 'hermes-stt-'));
const pcmFile = join(work, 'audio.f32');
execFileSync('ffmpeg', ['-nostdin', '-v', 'error', '-y', '-i', audioFile, '-ac', '1', '-ar', '16000',
  '-f', 'f32le', pcmFile], { stdio: ['ignore', 'inherit', 'inherit'] });

// The same policy as libpebble3's webview_startup.html.
const CSP = "script-src file: http: https: 'unsafe-eval' 'unsafe-hashes'; frame-src 'none'; " +
  "child-src 'none'; worker-src 'none'; object-src 'none'; connect-src file: http: https: data: blob:";
const pagePath = join(work, 'pkjs.html');
writeFileSync(pagePath, `<!DOCTYPE html><html><head>
<meta http-equiv="Content-Security-Policy" content="${CSP}">
<script src="file://${join(work, 'guard.js')}"></script>
</head><body></body></html>`);
// What realm_guard.js takes away.
writeFileSync(join(work, 'guard.js'),
  "for (const k of ['Worker','SharedWorker','RTCPeerConnection','WebTransport']) { try { delete window[k]; } catch (e) {} " +
  "Object.defineProperty(window, k, { value: undefined, configurable: false }); }");

const files = {
  '/engine.json': join(here, '../../src/pkjs/stt_engine.json'),
  '/runtime.wasm': join(here, 'node_modules/onnxruntime-web/dist/ort-wasm-simd-threaded.wasm'),
  '/audio.f32': pcmFile,
};
const server = createServer((request, response) => {
  const path = decodeURIComponent(new URL(request.url, 'http://x').pathname);
  const file = files[path] || normalize(join(cacheDir, path));
  if (!files[path] && !file.startsWith(normalize(cacheDir))) { response.writeHead(403).end(); return; }
  if (!existsSync(file)) { response.writeHead(404, { 'access-control-allow-origin': '*' }).end(); return; }
  response.writeHead(200, { 'access-control-allow-origin': '*', 'content-length': statSync(file).size });
  createReadStream(file).pipe(response);
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || undefined,
  // The WebView allows file pages to read anything (allowUniversalAccessFromFileURLs).
  args: ['--disable-web-security', '--allow-file-access-from-files'],
});
let failed = false;
try {
  const page = await browser.newPage();
  page.on('console', (m) => { if (process.env.STT_VERBOSE || m.type() === 'error' || m.type() === 'warning') console.log('[page]', m.text()); });
  page.on('pageerror', (e) => console.log('[pageerror]', e.message));
  await page.goto('file://' + pagePath);
  if (throttle > 1) {
    const cdp = await page.context().newCDPSession(page);
    await cdp.send('Emulation.setCPUThrottlingRate', { rate: throttle });
  }
  const result = await page.evaluate(async ({ base, repo, dtype, multilingual }) => {
    const get = (url, type) => new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('GET', url);
      xhr.responseType = type;
      xhr.onload = () => (xhr.status === 200 ? resolve(xhr.response) : resolve(null));
      xhr.onerror = () => reject(new Error('network error ' + url));
      xhr.send();
    });
    const out = { worker: typeof Worker, sab: typeof SharedArrayBuffer, idb: typeof indexedDB };
    console.log('stage: fetch engine');
    const engine = JSON.parse(await get(base + '/engine.json', 'text'));
    let t = performance.now();
    (0, eval)(engine.code);
    out.evalMs = Math.round(performance.now() - t);
    console.log('stage: engine evalled');
    const wasmBinary = await get(base + '/runtime.wasm', 'arraybuffer');
    console.log('stage: runtime fetched, loading model');
    t = performance.now();
    await HermesSTT.load({
      wasmBinary, repo, dtype,
      readFile: (path) => { console.log('stage: read ' + path); return get(base + '/' + repo + '/' + path, 'arraybuffer'); },
    });
    out.loadMs = Math.round(performance.now() - t);
    console.log('stage: model loaded');
    const samples = new Float32Array(await get(base + '/audio.f32', 'arraybuffer'));
    out.audioSeconds = +(samples.length / 16000).toFixed(1);
    // The first run pays for session warm-up; time a short clip and the whole clip separately.
    t = performance.now();
    out.firstThree = await HermesSTT.transcribe(samples.subarray(0, 48000), { multilingual, language: 'en' });
    out.firstThreeMs = Math.round(performance.now() - t);
    t = performance.now();
    out.text = await HermesSTT.transcribe(samples, { multilingual, language: 'en' });
    out.transcribeMs = Math.round(performance.now() - t);
    return out;
  }, { base, repo: model.repo, dtype: model.dtype, multilingual: model.multilingual });
  console.log(JSON.stringify({ model: modelId, throttle, ...result }, null, 1));
  if (!result.text || result.text.length < 10) failed = true;
} catch (error) {
  console.error('FAILED:', error.message);
  failed = true;
} finally {
  await browser.close();
  server.close();
}
process.exit(failed ? 1 : 0);
