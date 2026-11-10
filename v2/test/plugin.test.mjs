// The Hermes plugin (src/plugin/index.js) against the in-process mock Hermes server.
import test from 'node:test';
import assert from 'node:assert/strict';
import { loadPlugin, createClock } from './env.mjs';

const S = {
  QUEUED: 1, SUBMITTING: 2, ACCEPTED: 3, WORKING: 4, APPROVAL: 5, ANSWERED: 6,
  FAILED: 7, STOPPED: 8, CANCELLED: 9, UNKNOWN: 10, STOPPING: 11,
};
const HOUR = 3600000;

async function ready(options) {
  const p = loadPlugin(options);
  await p.setup();
  return p;
}

/** Submits a question and lets the mock finish it. */
async function ask(p, rid, text, extra = {}) {
  const first = await p.rpc('submit', Object.assign({ rid, text }, extra));
  p.clock.advance(5000);
  await p.rpc('poll');
  const item = await p.rpc('get', { rid, withText: true });
  return { first, item };
}

function posts(p) { return p.net.requests.filter((r) => r.method === 'POST' && r.path === '/v1/runs'); }

// ---- configuration -----------------------------------------------------------------------------

test('unconfigured: status says so and a question waits with the reason', async () => {
  const p = loadPlugin();
  const status = await p.rpc('status');
  assert.equal(status.configured, false);
  const item = await p.rpc('submit', { rid: 1, text: 'hello' });
  assert.equal(item.state, S.QUEUED);
  assert.equal(item.err, 'No server address yet');
  assert.equal(p.net.requests.length, 0);
});

test('save stores the URL normalised and the key only in secure storage', async () => {
  const p = loadPlugin();
  const snap = await p.config({ type: 'save', cfg: { url: '  https://hermes.test/v1/  ', brief: true }, apiKey: 'sk-secret-123' });
  assert.equal(snap.cfg.url, 'https://hermes.test');
  assert.equal(snap.cfg.brief, true);
  assert.equal(snap.hasKey, true);
  assert.equal(snap.hasExtra, false);
  assert.equal(p.secure.get('apiKey'), 'sk-secret-123');
  assert.ok(!JSON.stringify(snap).includes('sk-secret-123'), 'the snapshot never carries the secret');
  assert.ok(!p.storage.getItem('hp2.cfg').includes('sk-secret-123'), 'localStorage never holds the secret');
  assert.deepEqual(p.refreshed.at(-1), ['agent/configured']);
  const status = await p.rpc('status');
  assert.equal(status.configured, true);
  assert.equal(status.host, 'hermes.test');
});

test('save: an empty key keeps the saved one, clearApiKey removes it', async () => {
  const p = await ready();
  let snap = await p.config({ type: 'save', cfg: { brief: false }, apiKey: '' });
  assert.equal(snap.hasKey, true);
  assert.equal(p.secure.get('apiKey'), 'test-key');
  assert.equal(snap.cfg.url, 'https://hermes.test', 'fields not sent are left alone');
  snap = await p.config({ type: 'save', cfg: {}, clearApiKey: true });
  assert.equal(snap.hasKey, false);
  assert.equal((await p.rpc('status')).configured, false);
});

test('save ignores unknown fields and coerces idle hours', async () => {
  const p = await ready();
  const snap = await p.config({ type: 'save', cfg: { idleHours: '8', evil: 'x', extraName: '  X-Gate  ' } });
  assert.equal(snap.cfg.idleHours, 8);
  assert.equal(snap.cfg.extraName, 'X-Gate');
  assert.equal(snap.cfg.evil, undefined);
});

test('extra access header is sent when on, and can be cleared', async () => {
  const p = await ready();
  let snap = await p.config({ type: 'save', cfg: { extraOn: true, extraName: 'X-Gate' }, extraValue: 'gate-secret' });
  assert.equal(snap.hasExtra, true);
  assert.ok(!JSON.stringify(snap).includes('gate-secret'));
  await p.config({ type: 'test' });
  assert.equal(p.net.requests.at(-1).headers['x-gate'], 'gate-secret');
  assert.equal(p.net.requests.at(-1).headers.authorization, 'Bearer test-key');
  await p.config({ type: 'save', cfg: { extraOn: false } });
  await p.config({ type: 'test' });
  assert.equal(p.net.requests.at(-1).headers['x-gate'], undefined);
  snap = await p.config({ type: 'save', cfg: {}, clearExtra: true });
  assert.equal(snap.hasExtra, false);
});

test('test connection: success, wrong key, no URL, http rule, network, permission', async () => {
  const p = await ready();
  assert.deepEqual(await p.config({ type: 'test' }), { ok: true, message: 'Connected to hermes-agent' });
  const health = p.net.requests.filter((r) => r.path === '/health');
  assert.equal(health.length, 1);
  assert.ok(p.net.requests.every((r) => r.method === 'GET'), 'the test is read-only');

  await p.config({ type: 'save', cfg: {}, apiKey: 'wrong' });
  assert.deepEqual(await p.config({ type: 'test' }), { ok: false, message: 'Hermes is there, but rejected the API key' });

  await p.config({ type: 'save', cfg: { url: '' } });
  assert.equal((await p.config({ type: 'test' })).message, 'No server address yet');
  await p.config({ type: 'save', cfg: { url: 'hermes.test' } });
  assert.equal((await p.config({ type: 'test' })).message, 'The server address is not a URL');
  await p.config({ type: 'save', cfg: { url: 'http://hermes.test' } });
  assert.equal((await p.config({ type: 'test' })).message, 'The server address must start with https://');

  await p.config({ type: 'save', cfg: { url: 'https://hermes.test' }, apiKey: 'test-key' });
  p.net.fail = () => 'down';
  assert.deepEqual(await p.config({ type: 'test' }), { ok: false, message: 'Phone cannot reach Hermes' });
  p.net.fail = () => 'blocked';
  assert.deepEqual(await p.config({ type: 'test' }), { ok: false, message: 'Allow hermes.test for Hermes in the Pebble app' });
});

test('URL normalisation and the https rule', async () => {
  const { normalizeUrl } = loadPlugin().exports;
  assert.equal(normalizeUrl('https://h.example/'), 'https://h.example');
  assert.equal(normalizeUrl('https://h.example/v1'), 'https://h.example');
  assert.equal(normalizeUrl('https://h.example/V1//'), 'https://h.example');
  assert.equal(normalizeUrl(' https://h.example/hermes/v1/ '), 'https://h.example/hermes');
  assert.equal(normalizeUrl('https://h.example/v10'), 'https://h.example/v10');
  assert.equal(normalizeUrl(null), '');

  for (const [url, ok] of [
    ['http://localhost:8642', true], ['http://127.0.0.1:8642', true], ['http://10.0.2.2:8642', true],
    ['HTTPS://Hermes.Test', true], ['http://192.168.1.5:8642', false], ['ftp://hermes.test', false],
  ]) {
    const p = await ready();
    await p.config({ type: 'save', cfg: { url } });
    const result = await p.config({ type: 'test' });
    assert.equal(result.message === 'The server address must start with https://', !ok, url);
  }
});

test('a server under a path prefix is reached under it', async () => {
  const p = loadPlugin({ prefix: '/hermes' });
  await p.setup({ cfg: { url: 'https://hermes.test/hermes/v1/' } });
  assert.equal((await p.config({ type: 'test' })).ok, true);
  assert.equal(p.net.requests.at(-1).url, 'https://hermes.test/hermes/v1/models');
});

// ---- questions ---------------------------------------------------------------------------------

test('submit -> poll -> answered, with the answer made watch-friendly', async () => {
  const p = await ready();
  const item = await p.rpc('submit', { rid: 7, text: '  hello  ' });
  assert.equal(item.state, S.ACCEPTED);
  assert.equal(item.q, 'hello');
  assert.equal(item.src, 'watch');
  assert.equal(posts(p).length, 1);
  assert.match(posts(p)[0].headers['idempotency-key'], /^hp2-7-/);
  assert.equal(posts(p)[0].body.input, 'hello');
  assert.equal(posts(p)[0].body.session_id, undefined);

  p.clock.advance(2000);
  let poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.WORKING);
  assert.equal(poll.conv.active, true);
  p.clock.advance(3000);
  poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.ANSWERED);
  // Once answered it leaves the poll list.
  assert.deepEqual((await p.rpc('poll')).items, []);

  const got = await p.rpc('get', { rid: 7, withText: true });
  assert.equal(got.answer, 'You said: "hello". This is turn 1 of the conversation - all good.');
  assert.equal(got.bytes, Buffer.byteLength(got.answer));
  assert.equal((await p.rpc('get', { rid: 7 })).answer, undefined);
  assert.deepEqual(await p.rpc('get', { rid: 99 }), { missing: true, rid: 99 });
});

test('submit with no text is refused', async () => {
  const p = await ready();
  await assert.rejects(p.rpc('submit', { rid: 1, text: '   ' }), (e) => e.code === 'AUTH_REQUIRED' && /Nothing to send/.test(e.message));
});

test('resubmitting the same rid never makes a second run', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 5, text: 'hello' });
  await p.rpc('submit', { rid: 5, text: 'hello' });
  await p.rpc('submit', { rid: 5, text: 'something else' });
  assert.equal(p.mock.runs.size, 1);
  assert.equal(posts(p).length, 1);
});

test('a lost reply is retried with the same idempotency key and does not duplicate', async () => {
  const p = await ready();
  p.net.fail = (method) => (method === 'POST' ? 'drop' : null);
  const first = await p.rpc('submit', { rid: 11, text: 'hello' });
  assert.equal(first.state, S.QUEUED);
  assert.equal(first.err, 'Phone cannot reach Hermes');
  assert.equal(p.mock.runs.size, 1, 'the server did take it');
  p.net.fail = null;
  const again = await p.rpc('poll');
  assert.equal(again.items[0].state, S.ACCEPTED);
  assert.equal(p.mock.runs.size, 1);
  const keys = posts(p).map((r) => r.headers['idempotency-key']);
  assert.equal(keys.length, 2);
  assert.equal(keys[0], keys[1]);
});

test('turns of one conversation share the session', async () => {
  const p = await ready();
  await ask(p, 1, 'first');
  const { item } = await ask(p, 2, 'second');
  assert.equal(posts(p)[1].body.session_id, p.runsFor('first')[0].sessionId);
  assert.match(item.answer, /turn 2 of the conversation/);
  const status = await p.rpc('status');
  assert.equal(status.conv.turns, 2);
  assert.equal(status.conv.active, true);
});

for (const gap of [1000, 0]) {
  test(`a second question waits until the first has told us its session (${gap} ms apart)`, async () => {
    const p = await ready();
    // The run is accepted but the follow-up GET does not get through.
    p.net.fail = (method) => (method === 'GET' ? 'down' : null);
    assert.equal((await p.rpc('submit', { rid: 1, text: 'first' })).state, S.ACCEPTED);
    p.clock.advance(gap);
    const second = await p.rpc('submit', { rid: 2, text: 'second' });
    assert.equal(second.state, S.QUEUED);
    assert.equal(p.mock.runs.size, 1, 'not sent without the session');
    p.net.fail = null;
    const poll = await p.rpc('poll');
    assert.deepEqual(poll.items.map((i) => i.rid).sort(), [1, 2]);
    assert.equal(p.mock.runs.size, 2);
    assert.equal(p.runsFor('second')[0].sessionId, p.runsFor('first')[0].sessionId);
  });
}

test('a new conversation after the idle time, but not before or when off', async () => {
  const p = await ready();
  await ask(p, 1, 'first');
  p.clock.advance(3 * HOUR);
  await ask(p, 2, 'second');
  assert.equal(p.runsFor('second')[0].sessionId, p.runsFor('first')[0].sessionId);

  p.clock.advance(4 * HOUR + 60000);
  const status = await p.rpc('status');
  assert.equal(status.conv.active, false, 'the watch is told the conversation is over');
  await ask(p, 3, 'third');
  assert.notEqual(p.runsFor('third')[0].sessionId, p.runsFor('first')[0].sessionId);
  assert.equal(posts(p)[2].body.session_id, undefined);

  await p.config({ type: 'save', cfg: { idleHours: 0 } });
  p.clock.advance(30 * HOUR);
  await ask(p, 4, 'fourth');
  assert.equal(p.runsFor('fourth')[0].sessionId, p.runsFor('third')[0].sessionId);
});

test('newConv on submit and the newConversation call start fresh', async () => {
  const p = await ready();
  await ask(p, 1, 'first');
  await ask(p, 2, 'second', { newConv: true });
  assert.notEqual(p.runsFor('second')[0].sessionId, p.runsFor('first')[0].sessionId);
  const before = (await p.rpc('status')).conv.gen;
  const result = await p.rpc('newConversation');
  assert.equal(result.conv.gen, before + 1);
  assert.equal(result.conv.active, false);
  await ask(p, 3, 'third');
  assert.notEqual(p.runsFor('third')[0].sessionId, p.runsFor('second')[0].sessionId);
});

test('brief replies send instructions, otherwise none', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'plain' });
  assert.equal(p.runsFor('plain')[0].instructions, '');
  await p.config({ type: 'save', cfg: { brief: true } });
  await p.rpc('submit', { rid: 2, text: 'short please', newConv: true });
  assert.match(p.runsFor('short please')[0].instructions, /smartwatch/);
  assert.match(p.runsFor('short please')[0].instructions, /Markdown/);
});

test('a failed run reports Hermes\' error', async () => {
  const p = await ready();
  const { item } = await ask(p, 1, 'this will fail');
  assert.equal(item.state, S.FAILED);
  assert.equal(item.err, 'The tool crashed while reading your calendar');
});

test('a run waiting for approval says so and is never approved by us', async () => {
  const p = await ready();
  const { item } = await ask(p, 1, 'needs approval');
  assert.equal(item.state, S.APPROVAL);
  assert.ok(p.net.requests.every((r) => !/approv/.test(r.path)));
});

test('stop: a running request ends as stopped, a queued one as cancelled', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'a slow job' });
  p.clock.advance(20000);
  assert.equal((await p.rpc('poll')).items[0].state, S.WORKING);
  const stopping = await p.rpc('stop', { rid: 1 });
  assert.equal(stopping.state, S.STOPPING);
  assert.ok(p.net.requests.some((r) => r.method === 'POST' && /\/stop$/.test(r.path)));
  assert.equal((await p.rpc('poll')).items[0].state, S.STOPPED);

  p.net.fail = () => 'down';
  await p.rpc('submit', { rid: 2, text: 'never sent' });
  const cancelled = await p.rpc('stop', { rid: 2 });
  assert.equal(cancelled.state, S.CANCELLED);
  p.net.fail = null;
  await p.rpc('poll');
  assert.equal(p.runsFor('never sent').length, 0);
  assert.deepEqual(await p.rpc('stop', { rid: 77 }), { missing: true, rid: 77 });
});

test('a stop Hermes refuses keeps the state and says why', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'a slow job' });
  p.mock.runs.clear();                    // Hermes forgot the run
  const item = await p.rpc('stop', { rid: 1 });
  assert.equal(item.state, S.ACCEPTED);
  assert.equal(item.err, 'Unknown run');
});

test('401: the question waits with a clear reason, then goes once the key is fixed', async () => {
  const p = await ready();
  await p.config({ type: 'save', cfg: {}, apiKey: 'wrong' });
  const item = await p.rpc('submit', { rid: 1, text: 'hello' });
  assert.equal(item.state, S.QUEUED);
  assert.equal(item.err, 'Hermes rejected the API key');
  assert.equal(p.mock.runs.size, 0);
  await p.config({ type: 'save', cfg: {}, apiKey: 'test-key' });
  const poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.ACCEPTED);
  assert.equal(poll.items[0].err, '');
});

test('401 while polling a run keeps its state and reports the key', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'hello' });
  await p.config({ type: 'save', cfg: {}, apiKey: 'wrong' });
  const poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.ACCEPTED);
  assert.equal(poll.items[0].err, 'Hermes rejected the API key');
});

test('a session Hermes no longer knows is replaced by a new conversation', async () => {
  const p = await ready();
  await ask(p, 1, 'first');
  const conv = p.storage.json('hp2.conv');
  p.storage.setItem('hp2.conv', JSON.stringify(Object.assign(conv, { sessionId: 'sess_gone' })));
  const item = await p.rpc('submit', { rid: 2, text: 'second' });
  assert.equal(item.state, S.QUEUED);
  assert.equal(item.err, 'Started a new conversation');
  assert.equal(p.storage.json('hp2.conv').gen, conv.gen + 1);
  const poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.ACCEPTED);
  const run = p.runsFor('second')[0];
  assert.ok(run);
  assert.notEqual(run.sessionId, 'sess_gone');
  // The retry is a different request body, so it must not reuse the refused request's key.
  const keys = posts(p).slice(-2).map((r) => r.headers['idempotency-key']);
  assert.notEqual(keys[0], keys[1]);
});

test('a run Hermes no longer knows ends as unknown', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'hello' });
  p.mock.runs.clear();
  const poll = await p.rpc('poll');
  assert.equal(poll.items[0].state, S.UNKNOWN);
});

test('history keeps the newest twelve finished items and everything in flight', async () => {
  const p = await ready();
  for (let rid = 1; rid <= 15; rid++) {
    await ask(p, rid, 'question ' + rid);
    p.clock.advance(1000);
  }
  p.net.fail = () => 'down';
  await p.rpc('submit', { rid: 100, text: 'in flight' });
  const recent = await p.rpc('recent', { n: 50 });
  assert.equal(recent.items.length, 12);
  assert.equal(recent.items[0].rid, 100, 'newest first');
  assert.deepEqual(recent.items.slice(1).map((i) => i.rid), [15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5]);
  assert.deepEqual((await p.rpc('recent')).items.map((i) => i.rid), [100, 15, 14]);

  // Clear history removes finished items only.
  const snap = await p.config({ type: 'clearHistory' });
  assert.deepEqual(snap.items.map((i) => i.rid), [100]);
});

test('settings snapshot shows items with previews, notes, memos and speech state', async () => {
  const p = await ready();
  await ask(p, 1, 'hello');
  p.storage.setItem('hp2.notes', JSON.stringify([{ id: 5, at: 1, data: 'AAE=' }]));
  p.storage.setItem('hp2.memos', JSON.stringify([{ id: 9, at: 1, seconds: 3, status: 'sent', text: 'x' }]));
  p.storage.setItem('hp2.stt', JSON.stringify({ status: 'ready', model: 'moonshine-tiny' }));
  const snap = await p.config({ type: 'load' });
  assert.equal(snap.items[0].preview.startsWith('You said'), true);
  assert.equal(snap.notes.length, 1);
  assert.equal(snap.memos[0].status, 'sent');
  assert.equal(snap.stt.status, 'ready');
  assert.equal(snap.cfg.idleHours, 4);
  assert.equal(snap.cfg.sttMode, 'phone');
  const cleared = await p.config({ type: 'clearNotes' });
  assert.deepEqual(cleared.notes, []);
  const fresh = await p.config({ type: 'newConversation' });
  assert.equal(fresh.conv.active, false);
  assert.deepEqual(await p.config({ type: 'nonsense' }), { error: 'unknown message' });
});

test('clear history drops finished items and keeps the ones still open', async () => {
  const p = await ready();
  await ask(p, 1, 'hello');
  await ask(p, 2, 'needs approval');
  await ask(p, 3, 'please fail');
  assert.equal((await p.config({ type: 'load' })).items.length, 3);
  const snap = await p.config({ type: 'clearHistory' });
  assert.deepEqual(snap.items.map((i) => [i.rid, i.state]), [[2, S.APPROVAL]]);
  assert.equal((await p.rpc('get', { rid: 2 })).state, S.APPROVAL);
});

test('setSpeech is the one setting the watch changes', async () => {
  const p = await ready();
  assert.equal((await p.rpc('setSpeech', { mode: 'pebble' })).stt.mode, 'pebble');
  assert.equal((await p.rpc('setSpeech', { mode: 'anything' })).stt.mode, 'phone');
  await assert.rejects(p.rpc('nope'), (e) => e.code === 'INVALID_ARGS');
});

// ---- send_to_agent and sources -----------------------------------------------------------------

test('send_to_agent', async () => {
  const p = loadPlugin();
  assert.equal((await p.action('send_to_agent', { message: '  ' })).code, 'INVALID_ARGS');
  assert.equal((await p.action('send_to_agent', { message: 'hi' })).code, 'AUTH_REQUIRED');
  await p.setup();

  const sent = await p.action('send_to_agent', { message: 'remind me' });
  assert.deepEqual(sent, { ok: true, text: 'Sent to Hermes. Read the reply in the Hermes watch app.' });
  const recent = (await p.rpc('recent')).items;
  assert.equal(recent[0].src, 'index');
  assert.equal(recent[0].q, 'remind me');
  p.clock.advance(5000);
  await p.rpc('poll');

  await p.action('send_to_agent', { message: 'and this', chat_id: 'current' });
  assert.equal(p.runsFor('and this')[0].sessionId, p.runsFor('remind me')[0].sessionId);
  p.clock.advance(5000);
  await p.rpc('poll');
  await p.action('send_to_agent', { message: 'fresh start', chat_id: 'new' });
  assert.notEqual(p.runsFor('fresh start')[0].sessionId, p.runsFor('remind me')[0].sessionId);

  p.net.fail = () => 'down';
  assert.deepEqual(await p.action('send_to_agent', { message: 'offline' }),
    { ok: true, text: 'Saved for Hermes. It will send from the watch app.' });

  p.net.fail = null;
  p.mock.handle = () => [400, { error: { message: 'Input too long' } }];
  assert.deepEqual(await p.action('send_to_agent', { message: 'refused' }), { ok: false, code: 'UNKNOWN', message: 'Input too long' });
  assert.equal((await p.action('nothing', {})).code, 'INVALID_REQUEST');
});

test('sources: configured and chats', async () => {
  const p = loadPlugin();
  let r = await p.source('agent', 'configured');
  assert.equal(r.data.instances[0].properties.configured.boolean.value, false);
  await p.setup();
  r = await p.source('agent', 'configured');
  assert.equal(r.data.instances[0].properties.configured.boolean.value, true);
  assert.equal(r.data.instances[0].properties.configured.shortText.text, 'Ready');
  r = await p.source('agent', 'chats');
  assert.deepEqual(r.data.instances.map((i) => i.instanceId), ['current', 'new']);
  assert.equal((await p.source('weather', 'location')).code, 'PLUGIN_UNAVAILABLE');
});

// ---- text --------------------------------------------------------------------------------------

test('watchText strips Markdown and characters the watch cannot draw', () => {
  const { watchText } = loadPlugin().exports;
  const input = [
    '## Heading',
    '',
    'Some **bold**, __strong__, *italic* and `code` with a [link](https://x.example) and ![img](a.png).',
    '> quoted line',
    '- one',
    '* two',
    '+ three',
    '1. numbered stays',
    '',
    '---',
    '',
    '| Day | Hours |',
    '| --- | :---: |',
    '| Mon | 2 |',
    '',
    '```js',
    'const a_b = 1;',
    '```',
    'snake_case_word stays, 2*3*4 too.',
    '“Quotes” and ‘single’ – dash — em… arrow → done ✓ 🎉 emoji 中文 Zürich ł',
    '',
    '',
    '',
    'end   with   spaces   ',
  ].join('\r\n');
  const expected = [
    'Heading',
    '',
    'Some bold, strong, italic and code with a link and img.',
    'quoted line',
    '• one',
    '• two',
    '• three',
    '1. numbered stays',
    '',
    '',
    '',
    'Day · Hours',
    'Mon · 2',
    '',
    'const a_b = 1;',
    'snake_case_word stays, 2*3*4 too.',
    '"Quotes" and \'single\' - dash - em... arrow -> done ok emoji Zürich ł',
    '',
    'end with spaces',
  ].join('\n').replace(/\n{3,}/g, '\n\n');
  assert.equal(watchText(input), expected);
  assert.equal(watchText(null), '');
  assert.equal(watchText('   '), '');
});

test('watchText on the long mock answer leaves no Markdown behind', async () => {
  const p = await ready();
  const { item } = await ask(p, 1, 'tell me something long');
  assert.equal(item.state, S.ANSWERED);
  assert.doesNotMatch(item.answer, /[#|`]|\*\*/);
  assert.match(item.answer, /^Plan for the week/);
  assert.match(item.answer, /• Monday: dentist/);
  assert.match(item.answer, /Day · Free hours/);
  assert.match(item.answer, /Zürich at 07:15 - leave/);
});

test('outputText reads every answer shape the server may use', () => {
  const { outputText } = loadPlugin().exports;
  assert.equal(outputText({ output: 'plain' }), 'plain');
  assert.equal(outputText({ output: { text: 'obj text' } }), 'obj text');
  assert.equal(outputText({ output: { content: 'obj content' } }), 'obj content');
  assert.equal(outputText({ output: [{ type: 'message', content: [{ type: 'output_text', text: 'a' }, { type: 'output_text', text: 'b' }] }] }), 'a\n\nb');
  assert.equal(outputText({ output: [{ type: 'reasoning', text: 'hidden' }, { type: 'output_text', text: 'shown' }] }), 'shown');
  assert.equal(outputText({ output: ['x', { content: 'y' }] }), 'x\n\ny');
  assert.equal(outputText({ output: { content: [{ text: 'nested' }] } }), 'nested');
  assert.equal(outputText({}), '');
});

test('an empty answer still reads as finished', async () => {
  const p = await ready();
  await p.rpc('submit', { rid: 1, text: 'hello' });
  const handle = p.mock.handle;
  p.mock.handle = (method, path, headers, body) => {
    const r = handle(method, path, headers, body);
    if (method === 'GET' && r[1].status === 'completed') r[1].output = '```\n```';
    return r;
  };
  p.clock.advance(5000);
  await p.rpc('poll');
  assert.equal((await p.rpc('get', { rid: 1, withText: true })).answer, '(Hermes finished without a reply.)');
});

test('each plugin start sees the state the last one left', async () => {
  const clock = createClock();
  const first = loadPlugin({ clock });
  await first.setup();
  await first.rpc('submit', { rid: 3, text: 'hello' });
  const second = loadPlugin({ clock, storage: first.storage, secure: first.secure, mock: first.mock });
  clock.advance(5000);
  assert.equal((await second.rpc('poll')).items[0].state, S.ANSWERED);
});
