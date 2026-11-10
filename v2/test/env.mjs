// Test doubles for the phone side: the real plugin and PebbleKit JS code, run in a VM context
// with a fake Pebble object, an in-memory localStorage, a fake clock and the in-process mock
// Hermes server. Node built-ins only.

import vm from 'node:vm';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMock } from '../tools/mock-hermes.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
export const PLUGIN_PATH = join(root, 'src/plugin/index.js');
export const PKJS_PATH = join(root, 'src/pkjs/index.js');
const pluginSource = readFileSync(PLUGIN_PATH, 'utf8');
const pkjsSource = readFileSync(PKJS_PATH, 'utf8');

export const lib = createRequire(PKJS_PATH)('./lib');

export function memoryStorage() {
  const map = new Map();
  return {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => { map.set(k, String(v)); },
    removeItem: (k) => { map.delete(k); },
    clear: () => map.clear(),
    keys: () => [...map.keys()],
    json: (k) => (map.has(k) ? JSON.parse(map.get(k)) : null),
  };
}

export function createClock(start = Date.UTC(2026, 10, 10, 9, 0, 0)) {
  let t = start;
  return { now: () => t, advance(ms) { t += ms; }, set(v) { t = v; } };
}

function fakeDate(clock) {
  return class FakeDate extends Date {
    constructor(...args) { if (args.length) super(...args); else super(clock.now()); }
    static now() { return clock.now(); }
  };
}

/**
 * The plugin, as the Pebble app runs it. Every call goes to the same storage, so it behaves
 * like a plugin started afresh for each request.
 *
 * `net.fail` may be set to (method, path) => 'down' | 'drop' | 'blocked' | null:
 *   down     nothing reaches the server
 *   drop     the server handles it but the reply is lost
 *   blocked  the Pebble app's permission check refuses it
 */
export function loadPlugin(options = {}) {
  const clock = options.clock || createClock();
  const storage = options.storage || memoryStorage();
  const mock = options.mock || createMock(options.apiKey || 'test-key', clock.now);
  const secure = options.secure || new Map();
  const handlers = {};
  const refreshed = [];
  const net = { fail: null, requests: [], prefix: options.prefix || '' };

  async function fetch(url, init = {}) {
    const u = new URL(url);
    const method = init.method || 'GET';
    let path = u.pathname;
    if (net.prefix && path.startsWith(net.prefix)) path = path.slice(net.prefix.length);
    const headers = {};
    Object.entries(init.headers || {}).forEach(([k, v]) => { headers[k.toLowerCase()] = v; });
    const body = init.body ? JSON.parse(init.body) : {};
    net.requests.push({ method, url, path, headers, body });
    const failure = net.fail && net.fail(method, path);
    if (failure === 'blocked') { const e = new Error('blocked'); e.name = 'NotAllowedError'; throw e; }
    if (failure === 'down') throw new TypeError('Failed to fetch');
    const [status, payload] = mock.handle(method, path, headers, body);
    if (failure === 'drop') throw new TypeError('Network connection lost');
    return { status, text: async () => JSON.stringify(payload) };
  }

  const Pebble = {
    registerActionHandler: (f) => { handlers.action = f; },
    registerSourceHandler: (f) => { handlers.source = f; },
    registerConfigHandler: (f) => { handlers.config = f; },
    refreshSources: (list) => { refreshed.push([...list]); },
    secureStorage: {
      getItem: async (k) => (secure.has(k) ? secure.get(k) : null),
      setItem: async (k, v) => { secure.set(k, String(v)); },
      removeItem: async (k) => { secure.delete(k); },
    },
  };
  const context = vm.createContext({ Pebble, localStorage: storage, fetch, console, Date: fakeDate(clock) });
  vm.runInContext(pluginSource, context, { filename: PLUGIN_PATH });

  function action(name, args) {
    return new Promise((resolve) => {
      handlers.action({ action: name, args }, {
        ok: (r) => resolve(Object.assign({ ok: true }, r)),
        error: (code, message) => resolve({ ok: false, code, message }),
      });
    });
  }

  async function rpc(method, params) {
    const r = await action('watch_rpc', { method, params });
    if (!r.ok) throw Object.assign(new Error(r.message), r);
    return JSON.parse(r.text);
  }

  function config(message) {
    // Replies cross into the page as JSON; plain objects also compare cleanly in tests.
    return new Promise((resolve) => handlers.config(message, (r) => resolve(JSON.parse(JSON.stringify(r)))));
  }

  function source(category, item) {
    return new Promise((resolve) => {
      handlers.source({ category, item }, {
        data: (d) => resolve({ ok: true, data: JSON.parse(JSON.stringify(d)) }),
        error: (code, message) => resolve({ ok: false, code, message }),
      });
    });
  }

  /** Points the plugin at the mock, as the settings page would. */
  function setup(extra = {}) {
    return config(Object.assign({ type: 'save', cfg: { url: 'https://hermes.test' }, apiKey: 'test-key' }, extra));
  }

  return {
    Pebble, clock, storage, secure, mock, net, refreshed, action, rpc, config, source, setup,
    exports: context.__hermesPlugin,
    invokeAction: (o) => action(o.action, o.args),
    runsFor: (text) => [...mock.runs.values()].filter((r) => r.input === text),
  };
}

const tick = () => new Promise((resolve) => setImmediate(resolve));
export async function settle(rounds = 30) { for (let i = 0; i < rounds; i++) await tick(); }

/**
 * PebbleKit JS (src/pkjs/index.js) with a fake Pebble: AppMessages in and out, timers on the
 * fake clock, and invokeAction wired to a real plugin sharing its localStorage.
 */
export function loadPkjs(options = {}) {
  const clock = options.clock || createClock();
  const storage = options.storage || memoryStorage();
  const plugin = options.plugin || loadPlugin({ clock, storage });
  const listeners = {};
  const sent = [];
  const pushed = [];
  const timers = [];
  let nextTimer = 1;
  const faults = { appMessage: 0, rpc: 0 };

  const Pebble = {
    addEventListener: (name, f) => { (listeners[name] = listeners[name] || []).push(f); },
    sendAppMessage: (dict, ok, fail) => {
      const copy = JSON.parse(JSON.stringify(dict));
      setImmediate(() => {
        if (faults.appMessage > 0) { faults.appMessage--; fail({ error: 'nack' }); return; }
        sent.push(copy);
        ok({});
      });
    },
    invokeAction: (o) => {
      if (faults.rpc > 0) { faults.rpc--; return Promise.resolve({ ok: false, code: 'TIMEOUT', message: 'timed out' }); }
      return plugin.invokeAction(o);
    },
    sendConfigMessage: (m) => { pushed.push(m); },
  };
  const context = vm.createContext({
    Pebble, localStorage: storage, console, Date: fakeDate(clock),
    require: createRequire(PKJS_PATH),
    setTimeout: (fn, ms) => { const id = nextTimer++; timers.push({ id, at: clock.now() + (ms || 0), fn }); return id; },
    clearTimeout: (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
  });
  vm.runInContext(pkjsSource, context, { filename: PKJS_PATH });

  function emit(name, event) { (listeners[name] || []).forEach((f) => f(event)); }

  /** Lets the fake clock run for `ms`, firing timers as they fall due. */
  async function run(ms) {
    const until = clock.now() + ms;
    await settle();
    for (;;) {
      timers.sort((x, y) => x.at - y.at);
      const due = timers[0];
      if (!due || due.at > until) break;
      timers.shift();
      if (due.at > clock.now()) clock.set(due.at);
      due.fn();
      await settle();
    }
    clock.set(until);
    await settle();
  }

  async function watch(op, fields = {}) {
    emit('appmessage', { payload: Object.assign({ op }, fields) });
    await run(0);                          // and whatever it scheduled for straight away
  }

  return {
    Pebble, plugin, clock, storage, sent, pushed, faults, timers, emit, run, watch,
    of: (op) => sent.filter((m) => m.op === op),
    clear: () => { sent.length = 0; },
  };
}
