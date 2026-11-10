// Static checks on the settings page (src/config.html). The page is exercised for real in headless
// Chromium by tools/config-preview.mjs; these keep it honest without a browser.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const html = readFileSync(join(root, 'src/config.html'), 'utf8');
const read = (p) => readFileSync(join(root, p), 'utf8');
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);

test('the page script compiles', () => {
  assert.equal(scripts.length, 1);
  assert.doesNotThrow(() => new vm.Script(scripts[0], { filename: 'config.html' }));
});

test('the page loads nothing from outside itself', () => {
  assert.doesNotMatch(html, /<script[^>]+src=/i);
  assert.doesNotMatch(html, /<link\b/i);
  assert.doesNotMatch(html, /@import|url\(/i);
  assert.doesNotMatch(html, /\b(src|href|action)\s*=\s*["']?(https?:)?\/\//i);
  assert.doesNotMatch(scripts[0], /\bfetch\(|XMLHttpRequest|WebSocket/);
});

test('the page copy of the speech catalogue matches src/pkjs', () => {
  const catalog = JSON.parse(read('src/pkjs/stt_catalog.json'));
  const engine = JSON.parse(read('src/pkjs/stt_engine.json'));
  const models = vm.runInNewContext('(' + /var MODELS = (\[[\s\S]*?\]);/.exec(scripts[0])[1] + ')');
  assert.deepEqual(
    JSON.parse(JSON.stringify(models)),
    catalog.models.map((m) => ({ id: m.id, name: m.name, note: m.note, size: m.size, multilingual: m.multilingual })),
  );
  assert.equal(Number(/var RUNTIME_BYTES = (\d+);/.exec(scripts[0])[1]), engine.runtime.size);
});

test('the version shown is the app version', () => {
  const pkg = JSON.parse(read('package.json'));
  assert.equal(/var VERSION = '([^']+)';/.exec(scripts[0])[1], pkg.version);
});

test('every message the page sends has a handler on the other side', () => {
  const sent = (target) => new Set([...scripts[0].matchAll(new RegExp(`send\\('${target}', \\{ type: '(\\w+)'`, 'g'))].map((m) => m[1]));
  const plugin = read('src/plugin/index.js');
  const settings = /var settings = \{([\s\S]*?)\n {2}\};/.exec(plugin)[1];
  const handled = new Set([...settings.matchAll(/^ {4}(\w+): /gm)].map((m) => m[1]));
  const toPlugin = sent('plugin');
  assert.ok(toPlugin.size >= 6, [...toPlugin].join());
  for (const type of toPlugin) assert.ok(handled.has(type), `plugin handles ${type}`);

  const pkjs = read('src/pkjs/index.js');
  const toPkjs = sent('pkjs');
  assert.ok(toPkjs.size >= 3, [...toPkjs].join());
  for (const type of toPkjs) assert.match(pkjs, new RegExp(`message\\.type === '${type}'`), `pkjs handles ${type}`);
});

test('every control has a label', () => {
  const ids = [...html.matchAll(/<(?:input|select)\b[^>]*\bid="([^"]+)"/g)].map((m) => m[1]);
  assert.ok(ids.length >= 9, ids.join());
  for (const id of ids) assert.match(html, new RegExp(`for="${id}"`), `label for #${id}`);
});
