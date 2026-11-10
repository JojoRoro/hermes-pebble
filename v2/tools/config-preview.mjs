// Opens src/config.html in headless Chromium with a stand-in Pebble object that runs the real
// plugin (src/plugin/index.js) against the mock Hermes server, and a scripted PebbleKit JS side.
// Checks the page's main flows and writes phone-sized screenshots.
//
//   node tools/config-preview.mjs [out-dir]        (default /tmp/shots)
//
// Needs Chromium (CHROMIUM_PATH, or Playwright's download) and tools/stt's playwright-core
// (npm run engine installs it).

import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { mkdirSync, readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMock } from './mock-hermes.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const { chromium } = createRequire(join(here, 'stt/package.json'))('playwright-core');
const outDir = process.argv[2] || '/tmp/shots';
mkdirSync(outDir, { recursive: true });

const API_KEY = 'sk-preview-not-a-real-key';
const mock = createMock(API_KEY);
const server = createServer((request, response) => {
  const cors = {
    'access-control-allow-origin': '*',
    'access-control-allow-headers': '*',
    'access-control-allow-methods': 'GET, POST, OPTIONS',
  };
  if (request.method === 'OPTIONS') { response.writeHead(204, cors).end(); return; }
  let raw = '';
  request.on('data', (c) => { raw += c; });
  request.on('end', () => {
    let body = {};
    try { body = raw ? JSON.parse(raw) : {}; } catch (e) { /* not JSON */ }
    const [status, payload] = mock.handle(request.method, new URL(request.url, 'http://x').pathname, request.headers, body);
    response.writeHead(status, { 'content-type': 'application/json', ...cors });
    response.end(JSON.stringify(payload));
  });
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const hermesUrl = `http://127.0.0.1:${server.address().port}`;

const work = mkdtempSync(join(tmpdir(), 'hermes-config-'));
const pagePath = join(work, 'config.html');
writeFileSync(pagePath, readFileSync(join(root, 'src/config.html')));
const pluginSource = readFileSync(join(root, 'src/plugin/index.js'), 'utf8');

// ---- data a phone that has been used for a while would hold ------------------------------------

const now = Date.now();
function note(text) {
  // Crude strokes for a few letters, in the watch's format (120 x 128 grid).
  const shapes = {
    H: [[[25, 15], [25, 115]], [[95, 15], [95, 115]], [[25, 64], [95, 64]]],
    i: [[[60, 55], [60, 115]], [[60, 28], [61, 32]]],
    e: [[[30, 85], [90, 85], [85, 65], [60, 55], [35, 65], [28, 90], [45, 112], [85, 108]]],
    r: [[[35, 55], [35, 115]], [[35, 80], [55, 60], [85, 58]]],
    m: [[[20, 115], [20, 58]], [[20, 75], [40, 58], [60, 75], [60, 115]], [[60, 75], [80, 58], [100, 75], [100, 115]]],
    s: [[[88, 62], [60, 55], [35, 65], [45, 82], [80, 90], [88, 105], [60, 115], [30, 108]]],
    '2': [[[30, 35], [55, 15], [85, 25], [88, 50], [30, 115], [95, 115]]],
  };
  const bytes = [];
  for (const ch of text) {
    if (ch === ' ') { bytes.push(0xFE); continue; }
    (shapes[ch] || []).forEach((stroke, n) => {
      if (n) bytes.push(0xFD);
      stroke.forEach(([x, y]) => bytes.push(x, y));
    });
    bytes.push(0xFF);
  }
  return Buffer.from(bytes).toString('base64');
}

const seed = {
  'hp2.items': [
    { rid: 3, q: 'What is on my calendar this week?', src: 'watch', gen: 1, state: 6, err: '', runId: 'run_9', createdAt: now - 6 * 60000, updatedAt: now - 5 * 60000,
      answer: 'Plan for the week\n\nHere is what I found in your calendar and inbox:\n\n• Monday: dentist at 09:30, then the quarterly review at 14:00.\n• Tuesday: nothing fixed. A good day for the report.\n• Wednesday: flight to Zürich at 07:15 - leave by 05:30.' },
    { rid: 2, q: 'Book a table for two on Friday at the usual place', src: 'index', gen: 1, state: 5, err: '', runId: 'run_7', createdAt: now - 50 * 60000, updatedAt: now - 49 * 60000, answer: '' },
    { rid: 1, q: 'Summarise the unread mail from the bank', src: 'watch', gen: 1, state: 7, err: 'The tool crashed while reading your mail', runId: 'run_3', createdAt: now - 3 * 3600000, updatedAt: now - 3 * 3600000, answer: '' },
  ],
  'hp2.conv': { gen: 1, sessionId: 'sess_1', lastAt: now - 5 * 60000, turns: 3 },
  'hp2.notes': [
    { id: 11, at: now - 26 * 3600000, data: note('Hi Hermes') },
    { id: 12, at: now - 20 * 60000, data: note('Hermes 2 rr') },
  ],
  'hp2.memos': [
    { id: 21, at: Math.floor(now / 1000) - 7200, seconds: 14, status: 'sent', text: 'Remember to call the garage about the brakes before Thursday.' },
    { id: 22, at: Math.floor(now / 1000) - 600, seconds: 6, status: 'stored', text: '' },
  ],
  'hp2.stt': { supported: true, model: 'moonshine-tiny', name: 'Moonshine Tiny', bytes: 46344470, status: 'missing', percent: 0, error: '', at: now - 3600000 },
};

// ---- the stand-in Pebble -------------------------------------------------------------------------

function stub({ seed, pkjs, pluginSource }) {
  if (seed && !localStorage.getItem('hp2.seeded')) {
    Object.keys(seed).forEach((k) => localStorage.setItem(k, JSON.stringify(seed[k])));
    localStorage.setItem('hp2.seeded', '1');
  }
  const listeners = {};
  const handlers = {};
  const secure = JSON.parse(sessionStorage.getItem('secure') || '{}');
  const saveSecure = () => sessionStorage.setItem('secure', JSON.stringify(secure));
  const fire = (name, event) => (listeners[name] || []).forEach((f) => f(event));
  const models = { 'moonshine-tiny': 'Moonshine Tiny', 'moonshine-base': 'Moonshine Base', 'whisper-tiny': 'Whisper Tiny', 'whisper-base': 'Whisper Base' };
  const sttState = { supported: true, model: 'moonshine-tiny', name: 'Moonshine Tiny', bytes: 46344470, status: 'missing', percent: 0, error: '' };
  let timer = null;
  window.__log = [];

  function pkjsReply(message) {
    window.__log.push(['pkjs', message.type]);
    const cfg = JSON.parse(localStorage.getItem('hp2.cfg') || '{}');
    if (message.type === 'refresh' && cfg.sttModel && cfg.sttModel !== sttState.model) {
      clearInterval(timer);
      Object.assign(sttState, { model: cfg.sttModel, name: models[cfg.sttModel], status: 'missing', percent: 0 });
      setTimeout(() => fire('message', { data: { type: 'stt', stt: Object.assign({}, sttState) } }), 50);
    }
    if (message.type === 'sttDownload' && sttState.status !== 'downloading') {
      Object.assign(sttState, { status: 'downloading', percent: 0 });
      timer = setInterval(() => {
        sttState.percent = Math.min(100, sttState.percent + (window.__fast ? 25 : 1));
        if (sttState.percent >= 100) { clearInterval(timer); sttState.status = 'ready'; }
        fire('message', { data: { type: 'stt', stt: Object.assign({}, sttState) } });
      }, window.__fast ? 40 : 60000);
    }
    if (message.type === 'sttCancel') {
      clearInterval(timer);
      Object.assign(sttState, { status: 'missing', percent: 0 });
    }
    return { stt: Object.assign({}, sttState) };
  }

  window.Pebble = {
    // what the page uses
    addEventListener: (name, f) => { (listeners[name] = listeners[name] || []).push(f); },
    readyTargets: () => [],
    sendMessage: (target, message) => new Promise((resolve, reject) => {
      if (target === 'plugin') {
        window.__log.push(['plugin', message.type]);
        handlers.config(message, (reply) => resolve(JSON.parse(JSON.stringify(reply))));
      } else if (target === 'pkjs' && pkjs) {
        setTimeout(() => resolve(pkjsReply(message)), 20);
      } else {
        reject(new Error('target not running'));
      }
    }),
    // what the plugin uses
    registerActionHandler: (f) => { handlers.action = f; },
    registerSourceHandler: (f) => { handlers.source = f; },
    registerConfigHandler: (f) => { handlers.config = f; },
    refreshSources: () => {},
    secureStorage: {
      getItem: async (k) => (k in secure ? secure[k] : null),
      setItem: async (k, v) => { secure[k] = String(v); saveSecure(); },
      removeItem: async (k) => { delete secure[k]; saveSecure(); },
    },
  };
  window.__secure = secure;
  (0, eval)(pluginSource);
  window.__fire = fire;
  document.addEventListener('DOMContentLoaded', () => {
    setTimeout(() => fire('ready', { target: 'plugin' }), 30);
    if (pkjs) setTimeout(() => fire('ready', { target: 'pkjs' }), 60);
  });
}

// ---- run -----------------------------------------------------------------------------------------

const failures = [];
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures.push(what);
}

const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || '/root/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome',
  args: ['--disable-web-security', '--allow-file-access-from-files'],
});

async function open({ seed: data = null, pkjs = false, scale = 2 } = {}) {
  const context = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: scale, colorScheme: 'dark' });
  const page = await context.newPage();
  page.errors = [];
  page.on('pageerror', (e) => page.errors.push(e.message));
  page.on('console', (m) => {
    // The wrong-key test makes the browser log the mock's 401 itself; that is expected.
    if (m.type() === 'error' && !/status of 401/.test(m.text())) page.errors.push(m.text());
  });
  await page.addInitScript(stub, { seed: data, pkjs, pluginSource });
  await page.goto('file://' + pagePath);
  await page.waitForFunction(() => window.__log.some((e) => e[1] === 'load'));
  await page.waitForTimeout(150);
  return { page, context };
}

// textContent, not innerText: chips are upper-cased by CSS only.
const text = (page, sel) => page.locator(sel).evaluate((n) => n.textContent);

try {
  // 1. A fresh install: nothing set, the watch app not open.
  {
    const { page, context } = await open();
    check(/Enter your Hermes URL/.test(await text(page, '#banner')), 'fresh: banner asks for URL and key');
    check((await text(page, '#key-chip')) === 'Not set', 'fresh: key not set');
    check((await text(page, '#ready-chip')) === 'Set up', 'fresh: status chip says Set up');
    check(await page.locator('#dl-start').isDisabled(), 'fresh: download needs the watch app');
    await page.screenshot({ path: join(outDir, 'config-fresh.png') });

    await page.fill('#url', 'http://hermes.example.com');
    await page.click('#save');
    await page.waitForTimeout(100);
    check(/https:\/\//.test(await text(page, '#conn-status')), 'http to a remote host is refused with a reason');
    await page.screenshot({ path: join(outDir, 'config-url-error.png') });

    await page.fill('#url', hermesUrl + '/v1/');
    await page.fill('#apikey', 'wrong-key');
    await page.click('#test');
    await page.waitForFunction(() => /rejected/.test(document.getElementById('conn-status').textContent));
    check(true, 'test with a wrong key reports it');

    await page.fill('#apikey', API_KEY);
    await page.click('#test');
    await page.waitForFunction(() => /Connected/.test(document.getElementById('conn-status').textContent));
    check((await text(page, '#conn-status')).includes('Connected to hermes-agent'), 'test connection succeeds');
    check((await page.inputValue('#apikey')) === '', 'the key field is emptied after saving');
    check((await text(page, '#key-chip')) === 'Saved', 'key shows as saved');
    check((await page.inputValue('#url')) === hermesUrl, 'URL shown normalised');
    check(!(await page.content()).includes(API_KEY), 'the key is nowhere in the page');
    const stored = await page.evaluate(() => ({ cfg: localStorage.getItem('hp2.cfg'), key: window.__secure.apiKey }));
    check(stored.key === API_KEY && !stored.cfg.includes(API_KEY), 'key in secure storage only');
    check((await text(page, '#ready-chip')) === 'Ready', 'status chip says Ready');
    await page.screenshot({ path: join(outDir, 'config-connected.png') });

    // Extra header on, with a value, then the key removed.
    await page.check('#extra-on');
    await page.fill('#extra-name', 'X-Gate');
    await page.fill('#extra-value', 'gate-secret-value');
    await page.click('#save');
    await page.waitForFunction(() => document.getElementById('extra-chip').textContent === 'Saved');
    check(!(await page.content()).includes('gate-secret-value'), 'extra value never shown');
    await page.click('#key-clear');
    check((await text(page, '#key-chip')) === 'Will be removed', 'removing the key is staged');
    await page.click('#save');
    await page.waitForFunction(() => document.getElementById('key-chip').textContent === 'Not set');
    check(await page.evaluate(() => !('apiKey' in window.__secure)), 'key removed from secure storage');
    check(page.errors.length === 0, 'fresh: no page errors ' + page.errors.join(' | '));
    await context.close();
  }

  // 2. A phone in use, the watch app open: everything filled in.
  {
    const { page, context } = await open({ seed, pkjs: true, scale: 1 });
    await page.evaluate(([url, key]) => window.Pebble.sendMessage('plugin', { type: 'save', cfg: { url }, apiKey: key }), [hermesUrl, API_KEY]);
    await page.click('#reload');
    await page.waitForTimeout(200);
    check((await page.locator('#items li').count()) === 3, 'three recent items listed');
    check((await text(page, '#items')).includes('Needs approval in Hermes'), 'approval state labelled');
    check((await page.locator('#note-list canvas').count()) === 2, 'two notes drawn');
    const inked = await page.evaluate(() => {
      const c = document.querySelector('#note-list canvas');
      const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let dark = 0;
      for (let i = 0; i < d.length; i += 4) if (d[i] < 80 && d[i + 2] < 120) dark++;
      return dark;
    });
    check(inked > 200, 'note strokes are drawn (' + inked + ' ink pixels)');
    check((await text(page, '#memo-list')).includes('Sent to Hermes'), 'memo status shown');
    check((await text(page, '#conv-line')).includes('3 questions'), 'conversation line');
    check(!(await page.locator('#dl-start').isDisabled()), 'download possible with the watch app open');
    await page.waitForTimeout(2600);    // let the 'Up to date' toast go
    await page.screenshot({ path: join(outDir, 'config-full.png'), fullPage: true });

    // Behaviour saves at once.
    await page.check('#brief');
    await page.selectOption('#idle', '8');
    await page.waitForTimeout(100);
    const cfg = await page.evaluate(() => JSON.parse(localStorage.getItem('hp2.cfg')));
    check(cfg.brief === true && cfg.idleHours === 8, 'brief and idle saved straight away');

    // Clear history takes two taps and keeps the item still waiting for approval.
    await page.click('#clear-history');
    check((await text(page, '#clear-history')) === 'Tap again to clear', 'clear history asks again');
    check((await page.locator('#items li').count()) === 3, 'nothing cleared on the first tap');
    await page.click('#clear-history');
    await page.waitForTimeout(100);
    check((await page.locator('#items li').count()) === 1, 'finished items cleared, the open one kept');

    // Speech: pick Whisper Tiny, a language, download with live progress.
    await page.click('#models label:nth-child(3)');
    await page.waitForTimeout(150);
    check(await page.locator('#lang-field').isVisible(), 'language shown for a Whisper model');
    await page.selectOption('#lang', 'de');
    await page.waitForTimeout(100);
    const cfg2 = await page.evaluate(() => JSON.parse(localStorage.getItem('hp2.cfg')));
    check(cfg2.sttModel === 'whisper-tiny' && cfg2.sttLanguage === 'de', 'model and language saved');
    check((await text(page, '#dl-title')) === 'Whisper Tiny', 'download panel follows the model');
    await page.click('#dl-start');
    await page.waitForTimeout(100);
    await page.evaluate(() => window.__fire('message', { data: { type: 'stt', stt: { supported: true, model: 'whisper-tiny', name: 'Whisper Tiny', status: 'downloading', percent: 37, error: '' } } }));
    await page.waitForTimeout(350);
    check(/37%/.test(await text(page, '#dl-text')), 'live progress shown');
    check(await page.locator('#dl-cancel').isVisible(), 'cancel offered while downloading');
    await page.locator('#speech').screenshot({ path: join(outDir, 'config-speech.png') });
    await page.click('#dl-cancel');
    await page.waitForTimeout(100);
    check((await text(page, '#dl-chip')) === 'Not downloaded', 'cancel returns to not downloaded');
    await page.evaluate(() => { window.__fast = true; });
    await page.click('#dl-start');
    await page.waitForFunction(() => document.getElementById('dl-chip').textContent === 'On this phone', null, { timeout: 5000 });
    check(true, 'download completes to ready');

    await page.click('input[name=mode][value=pebble]');
    await page.waitForTimeout(100);
    check(await page.locator('#phone-opts').isHidden(), 'model options hidden for Pebble dictation');
    check((await page.evaluate(() => JSON.parse(localStorage.getItem('hp2.cfg')).sttMode)) === 'pebble', 'mode saved');

    await page.click('#clear-notes');
    await page.click('#clear-notes');
    await page.waitForTimeout(100);
    check((await page.locator('#note-list canvas').count()) === 0, 'notes cleared');
    check(page.errors.length === 0, 'in use: no page errors ' + page.errors.join(' | '));
    await page.locator('#behaviour').screenshot({ path: join(outDir, 'config-behaviour.png') });
    await context.close();
  }

  // 3. A phone whose Pebble app cannot run the model.
  {
    const unsupported = Object.assign({}, seed, { 'hp2.stt': Object.assign({}, seed['hp2.stt'], { supported: false, bytes: 0 }) });
    const { page, context } = await open({ seed: unsupported, scale: 1 });
    check((await text(page, '#dl-chip')) === 'Not supported', 'unsupported phone says so');
    check(page.errors.length === 0, 'unsupported: no page errors');
    await context.close();
  }
} finally {
  await browser.close();
  server.close();
}

console.log(failures.length ? `${failures.length} check(s) failed` : 'all checks passed', '-> screenshots in', outDir);
process.exit(failures.length ? 1 : 0);
