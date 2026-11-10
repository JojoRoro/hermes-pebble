// PebbleKit JS (src/pkjs/index.js) with a fake Pebble, wired to the real plugin and the mock.
import test from 'node:test';
import assert from 'node:assert/strict';
import { loadPkjs, lib } from './env.mjs';

const { W, P, PF, STATE } = lib;

async function bridge({ configured = true } = {}) {
  const pk = loadPkjs();
  if (configured) await pk.plugin.setup();
  pk.emit('ready', {});
  await pk.watch(W.HELLO);
  return pk;
}

test('ready, hello and state', async () => {
  const pk = await bridge();
  assert.deepEqual(pk.of(P.READY), [{ op: P.READY }]);
  const state = pk.of(P.STATE).at(-1);
  assert.equal(state.a & PF.CONFIGURED, PF.CONFIGURED);
  assert.equal(state.a & PF.STT_PHONE, PF.STT_PHONE);
  assert.equal(state.a & PF.STT_SUPPORT, 0, 'no on-phone speech under Node');
  assert.equal(state.txt, 'Moonshine Tiny');
  await pk.watch(W.PING, { a: 9 });
  assert.deepEqual(pk.of(P.PONG), [{ op: P.PONG, a: 9 }]);
});

test('unconfigured: the state says so', async () => {
  const pk = await bridge({ configured: false });
  assert.equal(pk.of(P.STATE).at(-1).a & PF.CONFIGURED, 0);
});

test('ask -> STORED -> ITEM -> DONE -> pages', async () => {
  const pk = await bridge();
  pk.clear();
  await pk.watch(W.ASK, { rid: 42, txt: 'tell me something long', a: 0 });
  assert.deepEqual(pk.sent[0], { op: P.STORED, rid: 42 });
  const first = pk.of(P.ITEM).filter((m) => m.rid === 42);
  assert.ok(first.length >= 1);
  assert.equal(first.at(-1).a, STATE.ACCEPTED);
  assert.equal(first.at(-1).txt, 'tell me something long');
  assert.deepEqual(pk.storage.json('hp2.outbox'), [], 'the plugin has it now');

  await pk.run(5000);
  const done = pk.of(P.DONE);
  assert.deepEqual(done, [{ op: P.DONE, rid: 42 }], 'one pulse');
  const answered = pk.of(P.ITEM).filter((m) => m.rid === 42).at(-1);
  assert.equal(answered.a, STATE.ANSWERED);
  assert.ok(answered.b > 1000, 'answer size');

  // Read it page by page, as the watch does.
  const full = (await pk.plugin.rpc('get', { rid: 42, withText: true })).answer;
  pk.clear();
  let offset = 0;
  const texts = [];
  for (let guard = 0; guard < 50 && offset < answered.b; guard++) {
    await pk.watch(W.OPEN, { rid: 42, a: offset });
    const page = pk.of(P.PAGE).at(-1);
    assert.equal(page.rid, 42);
    assert.equal(page.a, offset);
    assert.equal(page.b, answered.b);
    assert.ok(Buffer.byteLength(page.txt) <= lib.PAGE_BYTES);
    texts.push(page.txt);
    offset = page.c >> 1;
  }
  assert.ok(texts.length > 1, 'a long answer takes several pages');
  assert.equal(pk.of(P.PAGE)[0].c & 1, 1, 'the first page opens a paragraph');
  assert.equal(texts.join('').replace(/\s+/g, ''), full.replace(/\s+/g, ''));

  // Nothing more is polled once nothing is open.
  const before = pk.plugin.net.requests.length;
  await pk.run(60000);
  assert.equal(pk.plugin.net.requests.length, before);
});

test('opening an item without text reports it, and an unknown one as gone', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 5, txt: 'hello', a: 0 });
  await pk.run(5000);
  pk.clear();
  await pk.watch(W.OPEN, { rid: 5, a: -1 });
  assert.equal(pk.of(P.ITEM).at(-1).a, STATE.ANSWERED);
  assert.equal(pk.of(P.PAGE).length, 0);
  await pk.watch(W.OPEN, { rid: 6, a: 0 });
  assert.equal(pk.of(P.ITEM).at(-1).a, STATE.UNKNOWN);
});

test('asking twice with the same rid stores it once', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 8, txt: 'hello', a: 0 });
  await pk.watch(W.ASK, { rid: 8, txt: 'hello', a: 0 });
  assert.equal(pk.of(P.STORED).length, 2, 'both acknowledged');
  await pk.run(5000);
  assert.equal(pk.plugin.mock.runs.size, 1);
});

test('a follow-up continues the conversation; a=1 starts a new one', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 1, txt: 'first', a: 0 });
  await pk.run(5000);
  await pk.watch(W.ASK, { rid: 2, txt: 'second', a: 0 });
  await pk.run(5000);
  await pk.watch(W.ASK, { rid: 3, txt: 'third', a: 1 });
  await pk.run(5000);
  const [a, b, c] = ['first', 'second', 'third'].map((t) => pk.plugin.runsFor(t)[0].sessionId);
  assert.equal(a, b);
  assert.notEqual(b, c);
  assert.equal(pk.of(P.STATE).at(-1).a & PF.CONV_ACTIVE, PF.CONV_ACTIVE);
});

test('polling survives a plugin call that fails', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 9, txt: 'hello', a: 0 });
  assert.equal(pk.of(P.DONE).length, 0);
  pk.faults.rpc = 1;                       // the next poll times out
  await pk.run(30000);
  assert.deepEqual(pk.of(P.DONE), [{ op: P.DONE, rid: 9 }], 'the answer still arrives');
});

test('a question the plugin cannot take yet waits in the outbox and can be cancelled', async () => {
  const pk = await bridge();
  pk.faults.rpc = 1000;
  await pk.watch(W.ASK, { rid: 3, txt: 'later', a: 0 });
  assert.equal(pk.storage.json('hp2.outbox').length, 1);
  const waiting = pk.of(P.ITEM).at(-1);
  assert.equal(waiting.a, STATE.QUEUED);
  assert.equal(waiting.txt2, 'timed out');

  await pk.watch(W.RECENT, { a: 3 });
  const row = pk.of(P.RECENT).at(-1);
  assert.equal(row.rid, 3);
  assert.equal(row.c & 15, STATE.QUEUED);

  await pk.watch(W.STOP, { rid: 3 });
  assert.equal(pk.of(P.ITEM).at(-1).a, STATE.CANCELLED);
  assert.deepEqual(pk.storage.json('hp2.outbox'), []);
  pk.faults.rpc = 0;
  await pk.run(30000);
  assert.equal(pk.plugin.mock.runs.size, 0);
});

test('the outbox is sent once the plugin can take it', async () => {
  const pk = await bridge();
  pk.faults.rpc = 1;
  await pk.watch(W.ASK, { rid: 4, txt: 'retry me', a: 0 });
  assert.equal(pk.storage.json('hp2.outbox').length, 1);
  await pk.run(20000);
  assert.deepEqual(pk.storage.json('hp2.outbox'), []);
  assert.equal(pk.plugin.runsFor('retry me').length, 1);
  assert.equal(pk.of(P.DONE).length, 1);
});

test('recent lists the newest items, or says there are none', async () => {
  const pk = await bridge();
  await pk.watch(W.RECENT, { a: 3 });
  assert.deepEqual(pk.of(P.RECENT), [{ op: P.RECENT, a: -1, b: 0 }]);
  for (let rid = 1; rid <= 4; rid++) {
    await pk.watch(W.ASK, { rid, txt: 'question ' + rid, a: 0 });
    await pk.run(5000);
  }
  pk.clear();
  await pk.watch(W.RECENT, { a: 3 });
  const rows = pk.of(P.RECENT);
  assert.deepEqual(rows.map((r) => [r.a, r.b, r.rid, r.c & 15]), [[0, 3, 4, 6], [1, 3, 3, 6], [2, 3, 2, 6]]);
  assert.equal(rows[0].txt, 'question 4');
  assert.equal(rows[2].c >> 4, 15, 'age in seconds');
});

test('stop a running request from the watch', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 12, txt: 'a slow job', a: 0 });
  await pk.run(20000);
  await pk.watch(W.STOP, { rid: 12 });
  assert.equal(pk.of(P.ITEM).at(-1).a, STATE.STOPPING);
  await pk.run(3000);
  assert.equal(pk.of(P.ITEM).at(-1).a, STATE.STOPPED);
});

test('new conversation from the watch', async () => {
  const pk = await bridge();
  await pk.watch(W.ASK, { rid: 1, txt: 'first', a: 0 });
  await pk.run(5000);
  pk.clear();
  await pk.watch(W.NEWCONV);
  assert.equal(pk.of(P.STATE).at(-1).a & PF.CONV_ACTIVE, 0);
});

test('the watch switches speech to Pebble dictation and back', async () => {
  const pk = await bridge();
  await pk.watch(W.MODEL, { a: 3 });
  assert.equal(pk.of(P.STATE).at(-1).a & PF.STT_PHONE, 0);
  assert.equal(pk.storage.json('hp2.cfg').sttMode, 'pebble');
  await pk.watch(W.MODEL, { a: 4 });
  assert.equal(pk.of(P.STATE).at(-1).a & PF.STT_PHONE, PF.STT_PHONE);
});

// ---- memos and notes ---------------------------------------------------------------------------

function memoBytes(n) {
  return Array.from({ length: n }, (_, i) => (i * 131 + (i >> 3)) & 255);
}

async function sendMemo(pk, id, bytes, { crc = lib.crc32(bytes), chunk = 1000, skip = -1 } = {}) {
  await pk.watch(W.MEMO_BEGIN, { rid: id, a: bytes.length, b: Math.round(bytes.length / 4000), c: crc | 0, txt: '1762765200' });
  for (let offset = 0, n = 0; offset < bytes.length; offset += chunk, n++) {
    if (n === skip) continue;
    await pk.watch(W.MEMO_DATA, { rid: id, a: offset, bin: bytes.slice(offset, offset + chunk) });
  }
  await pk.watch(W.MEMO_END, { rid: id });
  return pk.of(P.MEMO_ACK).filter((m) => m.rid === id);
}

test('memo: received, checked against its crc, stored once', async () => {
  const pk = await bridge();
  const bytes = memoBytes(5200);
  const acks = await sendMemo(pk, 500, bytes);
  assert.deepEqual(acks[0], { op: P.MEMO_ACK, rid: 500, a: 0, b: 0 });
  assert.deepEqual(acks.at(-1), { op: P.MEMO_ACK, rid: 500, a: 1, b: 5200 });
  const memos = pk.storage.json('hp2.memos');
  assert.equal(memos.length, 1);
  assert.deepEqual([memos[0].id, memos[0].status, memos[0].at, memos[0].seconds], [500, 'stored', 1762765200, 1]);
  assert.deepEqual(lib.base64Decode(pk.storage.getItem('hp2.memo.500')), bytes);

  // The watch did not hear the ack and starts again: told it is already here.
  pk.clear();
  await pk.watch(W.MEMO_BEGIN, { rid: 500, a: 5200, b: 1, c: lib.crc32(bytes) | 0, txt: '1762765200' });
  assert.deepEqual(pk.of(P.MEMO_ACK), [{ op: P.MEMO_ACK, rid: 500, a: 1, b: 0 }]);
  assert.equal(pk.storage.json('hp2.memos').length, 1);
});

test('memo: a crc above 2^31 sent as a negative int32 still matches', async () => {
  const pk = await bridge();
  let bytes = memoBytes(300);
  for (let i = 0; lib.crc32(bytes) <= 0x7fffffff; i++) bytes = memoBytes(300 + i);
  const acks = await sendMemo(pk, 501, bytes);
  assert.equal(acks.at(-1).a, 1);
});

test('memo: damaged or incomplete is refused and asked for again', async () => {
  const pk = await bridge();
  const bytes = memoBytes(3000);
  let acks = await sendMemo(pk, 600, bytes, { crc: lib.crc32(bytes) ^ 1 });
  assert.equal(acks.at(-1).a, 2);
  acks = await sendMemo(pk, 601, bytes, { skip: 1 });
  assert.equal(acks.at(-1).a, 2);
  assert.equal(pk.storage.json('hp2.memos'), null);
  acks = await sendMemo(pk, 600, bytes);
  assert.equal(acks.at(-1).a, 1);
});

test('memo: an interrupted transfer resumes where it stopped', async () => {
  const pk = await bridge();
  const bytes = memoBytes(3000);
  await pk.watch(W.MEMO_BEGIN, { rid: 700, a: 3000, b: 1, c: lib.crc32(bytes) | 0, txt: '1762765200' });
  await pk.watch(W.MEMO_DATA, { rid: 700, a: 0, bin: bytes.slice(0, 1000) });
  pk.clear();
  await pk.watch(W.MEMO_BEGIN, { rid: 700, a: 3000, b: 1, c: lib.crc32(bytes) | 0, txt: '1762765200' });
  assert.deepEqual(pk.of(P.MEMO_ACK), [{ op: P.MEMO_ACK, rid: 700, a: 0, b: 1000 }]);
  await pk.watch(W.MEMO_DATA, { rid: 700, a: 1000, bin: bytes.slice(1000) });
  await pk.watch(W.MEMO_END, { rid: 700 });
  assert.equal(pk.of(P.MEMO_ACK).at(-1).a, 1);
});

test('note: strokes are stored base64 and acknowledged', async () => {
  const pk = await bridge();
  const note = [10, 10, 60, 100, 0xFD, 20, 50, 80, 50, 0xFF, 0xFE, 30, 20, 30, 110, 0xFF];
  const crc = lib.crc32(note) | 0;
  await pk.watch(W.NOTE, { rid: 33, a: 0, b: note.length, c: crc, bin: note.slice(0, 9) });
  assert.equal(pk.of(P.NOTE_ACK).length, 0);
  await pk.watch(W.NOTE, { rid: 33, a: 9, b: note.length, c: crc, bin: note.slice(9) });
  assert.deepEqual(pk.of(P.NOTE_ACK), [{ op: P.NOTE_ACK, rid: 33 }]);
  const notes = pk.storage.json('hp2.notes');
  assert.equal(notes.length, 1);
  assert.equal(notes[0].id, 33);
  assert.deepEqual(lib.base64Decode(notes[0].data), note);
  // The settings page reads them through the plugin.
  assert.equal((await pk.plugin.config({ type: 'load' })).notes[0].data, notes[0].data);

  // A damaged note is not acknowledged, so the watch keeps it.
  pk.clear();
  await pk.watch(W.NOTE, { rid: 34, a: 0, b: note.length, c: crc ^ 1, bin: note });
  assert.equal(pk.of(P.NOTE_ACK).length, 0);
});

// ---- settings page ---------------------------------------------------------------------------------

test('the settings page gets the speech state and can ask for a refresh', async () => {
  const pk = await bridge();
  let reply;
  pk.emit('configmessage', { data: { type: 'refresh' }, respond: (r) => { reply = r; } });
  await pk.run(0);
  assert.equal(reply.stt.supported, false);
  assert.equal(reply.stt.model, 'moonshine-tiny');
  assert.equal(reply.stt.status, 'missing');
  // A model chosen on the page is picked up on refresh.
  await pk.plugin.config({ type: 'save', cfg: { sttModel: 'whisper-tiny' } });
  pk.clear();
  pk.emit('configmessage', { data: { type: 'refresh' }, respond: (r) => { reply = r; } });
  await pk.run(0);
  assert.equal(pk.of(P.STATE).at(-1).txt, 'Whisper Tiny');
});

test('a failed AppMessage is retried', async () => {
  const pk = await bridge();
  pk.clear();
  pk.faults.appMessage = 2;
  await pk.watch(W.PING, { a: 1 });
  await pk.run(2000);
  assert.deepEqual(pk.of(P.PONG), [{ op: P.PONG, a: 1 }]);
});
