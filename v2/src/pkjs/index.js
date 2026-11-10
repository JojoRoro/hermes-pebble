// PebbleKit JS: the bridge between the watch app and the Hermes plugin.
//
// It runs only while the watch app is open. It carries AppMessages, drives the polling a plugin
// cannot do for itself, and hosts the on-phone speech engine, because the watch's microphone
// stream is delivered here. Everything that touches Hermes goes through the plugin.
'use strict';

var lib = require('./lib');
var Stt = require('./stt');

var W = lib.W, P = lib.P, PF = lib.PF, STATE = lib.STATE;
var UUID = '16f5cba8-2d99-4173-9e41-6d19458e1b5a';

// ---- to the watch --------------------------------------------------------------------------

var queue = [];
var sending = false;

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  var entry = queue[0];
  Pebble.sendAppMessage(entry.dict, function () {
    queue.shift();
    sending = false;
    pump();
  }, function () {
    sending = false;
    if (++entry.tries >= 4) queue.shift();
    setTimeout(pump, 400);
  });
}

/** Queues a message. One with a tag replaces a waiting message with the same tag. */
function send(op, fields, tag) {
  var dict = { op: op };
  Object.keys(fields || {}).forEach(function (k) { dict[k] = fields[k]; });
  if (tag) {
    for (var i = sending ? 1 : 0; i < queue.length; i++) {
      if (queue[i].tag === tag) {
        queue[i].dict = dict;
        return;
      }
    }
  }
  queue.push({ dict: dict, tag: tag || '', tries: 0 });
  pump();
}

// ---- to the plugin -------------------------------------------------------------------------

function rpc(method, params) {
  if (typeof Pebble.invokeAction !== 'function') {
    return Promise.reject({ code: 'NO_PLUGINS', message: 'Turn on plugins in the Pebble app settings' });
  }
  return Pebble.invokeAction({
    plugin: UUID,
    action: 'watch_rpc',
    args: { method: method, params: params || {} },
    timeoutMs: 20000,
  }).then(function (result) {
    if (result && result.ok) return JSON.parse(result.text || '{}');
    throw result || { code: 'UNKNOWN', message: 'The Hermes plugin did not answer' };
  });
}

function problemText(e) {
  if (!e) return 'The Hermes plugin did not answer';
  if (e.code === 'PERMISSION_DENIED') return 'Allow Hermes 2 its permissions in the Pebble app';
  if (e.code === 'PLUGIN_UNAVAILABLE') return 'Turn on plugins in the Pebble app settings';
  return String(e.message || e.code || e);
}

function readJson(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) : fallback;
  } catch (e) {
    return fallback;
  }
}

function writeJson(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) { /* full */ }
}

// ---- state ---------------------------------------------------------------------------------

var status = null;                 // the plugin's last status
var stt = new Stt(function () { sttChanged(); });
var known = {};                    // rid -> last state sent to the watch
var answers = {};                  // rid -> answer bytes
var lastSubmit = 0;
var pollTimer = null;
var polling = false;

function stateFlags() {
  var flags = 0;
  if (status && status.configured) flags |= PF.CONFIGURED;
  if (status && status.conv && status.conv.active) flags |= PF.CONV_ACTIVE;
  if (Stt.supported()) flags |= PF.STT_SUPPORT;
  if (!status || status.stt.mode !== 'pebble') flags |= PF.STT_PHONE;
  if (stt.status === 'ready') flags |= PF.MODEL_READY;
  if (stt.status === 'downloading') flags |= PF.DOWNLOADING;
  if (stt.status === 'error') flags |= PF.MODEL_ERROR;
  return flags;
}

function sendState() {
  send(P.STATE, {
    a: stateFlags(),
    b: stt.percent,
    c: status && status.conv ? status.conv.idleMin : -1,
    txt: stt.model.name,
    txt2: Stt.supported() ? lib.megabytes(stt.totalBytes()) : '',
  }, 'state');
}

function sttChanged() {
  var summary = stt.summary();
  summary.at = Date.now();
  writeJson('hp2.stt', summary);
  sendState();
  // Live progress for the settings page, if it is open; harmless otherwise.
  if (typeof Pebble.sendConfigMessage === 'function') {
    try { Pebble.sendConfigMessage({ type: 'stt', stt: summary }); } catch (e) { /* no page */ }
  }
  if (stt.status === 'ready') processMemos();
}

function refreshStatus() {
  return rpc('status').then(function (result) {
    status = result;
    return stt.configure(result.stt.model, result.stt.language);
  }).then(sendState, function (e) {
    // The watch still needs to hear from us, or it keeps knocking.
    sendState();
    send(P.TOAST, { txt: problemText(e) }, 'toast');
  });
}

// ---- questions -----------------------------------------------------------------------------

// Questions the phone holds but the plugin has not taken yet.
function outbox() { return readJson('hp2.outbox', []); }

function sendItem(item) {
  known[item.rid] = item.state;
  send(P.ITEM, {
    rid: item.rid,
    a: item.state,
    b: item.bytes || 0,
    c: Math.max(0, Math.floor((Date.now() - (item.createdAt || Date.now())) / 1000)),
    txt: lib.clipBytes(item.q || '', 180),
    txt2: lib.clipBytes(item.err || '', 110),
  }, 'item' + item.rid);
}

function report(item) {
  var before = known[item.rid];
  if (before === item.state && !item.err) return;
  // One pulse when an answer lands while the user is waiting for it.
  if (item.state === STATE.ANSWERED && before !== undefined && before !== STATE.ANSWERED) {
    send(P.DONE, { rid: item.rid });
  }
  if (item.state === STATE.ANSWERED) delete answers[item.rid];
  sendItem(item);
}

function flushOutbox() {
  var pending = outbox();
  if (!pending.length) return Promise.resolve();
  var entry = pending[0];
  return rpc('submit', entry).then(function (item) {
    writeJson('hp2.outbox', outbox().filter(function (e) { return e.rid !== entry.rid; }));
    report(item);
    return flushOutbox();
  }, function (e) {
    sendItem({ rid: entry.rid, state: STATE.QUEUED, q: entry.text, err: problemText(e), createdAt: entry.at });
  });
}

function schedulePoll(delay) {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = setTimeout(pollOnce, delay);
}

function pollOnce() {
  pollTimer = null;
  if (polling) return;
  polling = true;
  flushOutbox().then(function () { return rpc('poll'); }).then(function (result) {
    polling = false;
    var open = 0;
    (result.items || []).forEach(function (item) {
      report(item);
      if (!lib.isFinal(item.state)) open++;
    });
    var conv = result.conv;
    if (status && conv && (conv.gen !== status.conv.gen || conv.active !== status.conv.active)) {
      status.conv = conv;
      sendState();
    } else if (status && conv) {
      status.conv = conv;
    }
    if (open || outbox().length) {
      var age = Date.now() - lastSubmit;
      schedulePoll(age < 30000 ? 2000 : age < 180000 ? 4000 : 8000);
    }
  }, function () {
    polling = false;
    // One failed call (a plugin timeout, say) must not end the wait for an answer.
    var waiting = Object.keys(known).some(function (rid) { return !lib.isFinal(known[rid]); });
    if (waiting || outbox().length) schedulePoll(8000);
  });
}

function onAsk(m) {
  var entry = { rid: m.rid, text: String(m.txt || ''), newConv: !!(m.a & 1), src: 'watch', at: Date.now() };
  // Held here first, so the watch can let go of its copy whatever the plugin is doing.
  var pending = outbox();
  if (!pending.some(function (e) { return e.rid === entry.rid; }) && known[entry.rid] === undefined) {
    pending.push(entry);
    writeJson('hp2.outbox', pending);
  }
  send(P.STORED, { rid: m.rid });
  if (known[entry.rid] === undefined) known[entry.rid] = STATE.QUEUED;
  lastSubmit = Date.now();
  schedulePoll(0);
}

function sendPage(rid, offset) {
  var bytes = answers[rid];
  var page = lib.pageAt(bytes, offset, lib.PAGE_BYTES);
  send(P.PAGE, {
    rid: rid, a: offset, b: bytes.length, c: (page.next << 1) | (page.paragraph ? 1 : 0), txt: page.text,
  });
}

function onOpen(m) {
  var rid = m.rid;
  var waiting = outbox().filter(function (e) { return e.rid === rid; })[0];
  if (waiting) {
    sendItem({ rid: rid, state: STATE.QUEUED, q: waiting.text, createdAt: waiting.at });
    return;
  }
  if (m.a >= 0 && answers[rid]) {
    sendPage(rid, m.a);
    return;
  }
  rpc('get', { rid: rid, withText: m.a >= 0 }).then(function (item) {
    if (item.missing) {
      sendItem({ rid: rid, state: STATE.UNKNOWN, err: 'The phone no longer has this one' });
      return;
    }
    sendItem(item);
    if (m.a >= 0 && item.state === STATE.ANSWERED) {
      answers[rid] = lib.utf8Encode(item.answer || '');
      sendPage(rid, m.a);
    }
    if (!lib.isFinal(item.state) && !pollTimer) schedulePoll(1000);
  }, function (e) {
    send(P.TOAST, { txt: problemText(e) }, 'toast');
  });
}

function onStop(m) {
  var pending = outbox();
  var waiting = pending.filter(function (e) { return e.rid === m.rid; })[0];
  if (waiting) {
    writeJson('hp2.outbox', pending.filter(function (e) { return e.rid !== m.rid; }));
    sendItem({ rid: m.rid, state: STATE.CANCELLED, q: waiting.text, createdAt: waiting.at });
    return;
  }
  rpc('stop', { rid: m.rid }).then(function (item) {
    if (!item.missing) sendItem(item);
    schedulePoll(1500);
  }, function (e) {
    send(P.TOAST, { txt: problemText(e) }, 'toast');
  });
}

function onRecent(m) {
  var limit = m.a > 0 ? m.a : 3;
  rpc('recent', { n: limit }).then(function (result) { return result.items || []; }, function () { return []; })
    .then(function (items) {
      var rows = outbox().map(function (e) {
        return { rid: e.rid, state: STATE.QUEUED, q: e.text, createdAt: e.at };
      }).reverse().concat(items).slice(0, limit);
      if (!rows.length) {
        send(P.RECENT, { a: -1, b: 0 });
        return;
      }
      rows.forEach(function (item, index) {
        var age = Math.max(0, Math.floor((Date.now() - item.createdAt) / 1000));
        send(P.RECENT, {
          a: index, b: rows.length, rid: item.rid,
          c: (Math.min(age, 0x7ffffff) << 4) | (item.state & 15),
          txt: lib.clipBytes(item.q || '', 68),
        });
      });
    });
}

// ---- listening -----------------------------------------------------------------------------

var session = null;
var dropNext = false;              // the watch cancelled before the stream reached us

function gather(chunks, maxSamples) {
  var total = 0;
  chunks.forEach(function (c) { total += c.length; });
  var skip = maxSamples && total > maxSamples ? total - maxSamples : 0;
  var out = new Float32Array(total - skip);
  var at = 0;
  chunks.forEach(function (c) {
    if (skip >= c.length) { skip -= c.length; return; }
    var part = skip ? c.subarray(skip) : c;
    skip = 0;
    out.set(part, at);
    at += part.length;
  });
  return out;
}

function maybePartial(s) {
  if (s.busy || s.done || s.cancelled || !s.vad.heard || !stt.isLoaded()) return;
  // Leave the phone room to breathe: wait for new audio worth at least the last run's cost.
  var need = Math.max(16000, s.lastCost * 16 * 1.5);
  if (s.samples - s.partialAt < need) return;
  s.busy = true;
  s.partialAt = s.samples;
  var started = Date.now();
  stt.transcribe(gather(s.chunks, 25 * 16000)).then(function (text) {
    s.busy = false;
    s.lastCost = Date.now() - started;
    if (s !== session || s.cancelled) return;
    if (s.done) { finalize(s); return; }
    if (text) send(P.PARTIAL, { txt: lib.clipBytes(text, lib.TEXT_BYTES) }, 'partial');
    maybePartial(s);
  }, function () {
    s.busy = false;
    if (s.done && s === session) finalize(s);
  });
}

function finalize(s) {
  if (s.cancelled || s.finalized) return;
  s.finalized = true;
  if (!s.vad.heard || s.samples < 4800) {
    send(P.FINAL, { a: 1, txt: '' });
    return;
  }
  if (!stt.usable()) {
    send(P.FINAL, { a: 2, txt: '' });
    return;
  }
  var parts = lib.splitForModel(gather(s.chunks), 25);
  var texts = [];
  var step = function (index) {
    if (index >= parts.length) return Promise.resolve();
    return stt.transcribe(parts[index]).then(function (text) {
      if (text) texts.push(text);
      return step(index + 1);
    });
  };
  step(0).then(function () {
    if (s.cancelled) return;
    var text = texts.join(' ').trim();
    send(P.FINAL, { a: text ? 0 : 1, txt: lib.clipBytes(text, lib.TEXT_BYTES) });
  }, function (e) {
    console.log('transcription failed: ' + ((e && e.message) || e));
    if (!s.cancelled) send(P.FINAL, { a: 2, txt: '' });
  });
}

Pebble.addEventListener('audiostreamstart', function () {
  session = {
    chunks: [], samples: 0, partialAt: 0, lastCost: 0, busy: false, done: false,
    cancelled: dropNext, finalized: false, vadSent: false,
    vad: new lib.Vad({ pauseMs: 1600 }),
  };
  dropNext = false;
  if (stt.usable()) stt.ensureLoaded().catch(function () {});
});

Pebble.addEventListener('audiostream', function (e) {
  var s = session;
  if (!s || s.done || s.cancelled) return;
  var pcm = new Int16Array(e.data);
  var samples = new Float32Array(pcm.length);
  for (var i = 0; i < pcm.length; i++) samples[i] = pcm[i] / 32768;
  s.chunks.push(samples);
  s.samples += samples.length;
  s.vad.feed(samples);
  var autoStop = !status || status.stt.autoStop !== false;
  if (autoStop && !s.vadSent && s.vad.ended()) {
    s.vadSent = true;
    send(P.VAD, {});
  }
  maybePartial(s);
});

Pebble.addEventListener('audiostreamstop', function () {
  var s = session;
  if (!s || s.done) return;
  s.done = true;
  if (!s.busy) finalize(s);
});

function onListen(m) {
  if (m.a) {
    dropNext = false;
    if (stt.usable()) stt.ensureLoaded().catch(function () {});
  } else if (session && !session.finalized) {
    session.cancelled = true;
  } else {
    dropNext = !session || session.done;
  }
}

// ---- memos ---------------------------------------------------------------------------------

var incoming = null;               // the memo being received
var memoBusy = false;

function memoList() { return readJson('hp2.memos', []); }

function saveMemoAudio(id, bytes) {
  if (Stt.supported()) return Stt.idb.put('memo/' + id, bytes.buffer);
  try {
    localStorage.setItem('hp2.memo.' + id, lib.base64Encode(bytes));
    return Promise.resolve();
  } catch (e) {
    return Promise.reject(e);
  }
}

function loadMemoAudio(id) {
  if (Stt.supported()) {
    return Stt.idb.get('memo/' + id).then(function (b) { return b ? new Uint8Array(b) : null; });
  }
  var raw = localStorage.getItem('hp2.memo.' + id);
  return Promise.resolve(raw ? new Uint8Array(lib.base64Decode(raw)) : null);
}

function dropMemoAudio(id) {
  try { localStorage.removeItem('hp2.memo.' + id); } catch (e) { /* nothing there */ }
  if (Stt.supported()) return Stt.idb.del('memo/' + id).catch(function () {});
  return Promise.resolve();
}

function updateMemo(id, changes) {
  var list = memoList();
  list.forEach(function (memo) {
    if (memo.id === id) Object.keys(changes).forEach(function (k) { memo[k] = changes[k]; });
  });
  writeJson('hp2.memos', list.slice(-20));
}

function memoTime(seconds) {
  var d = new Date(seconds * 1000);
  var two = function (n) { return (n < 10 ? '0' : '') + n; };
  return d.getFullYear() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' +
    two(d.getHours()) + ':' + two(d.getMinutes());
}

/** Turns stored memos into text and hands them to Hermes, oldest first. */
function processMemos() {
  if (memoBusy || !stt.usable()) return;
  var memo = memoList().filter(function (m) { return m.status === 'stored'; })[0];
  if (!memo) return;
  memoBusy = true;
  loadMemoAudio(memo.id).then(function (bytes) {
    if (!bytes) {
      updateMemo(memo.id, { status: 'lost' });
      return null;
    }
    var parts = lib.splitForModel(lib.memoToFloat16k(bytes), 25);
    var texts = [];
    var step = function (index) {
      if (index >= parts.length) return Promise.resolve(texts.join(' ').trim());
      return stt.transcribe(parts[index]).then(function (text) {
        if (text) texts.push(text);
        return step(index + 1);
      });
    };
    return step(0).then(function (text) {
      if (!text) {
        updateMemo(memo.id, { status: 'empty' });
        return dropMemoAudio(memo.id);
      }
      updateMemo(memo.id, { status: 'transcribed', text: text });
      // The memo's id is the request id, so sending it twice cannot make two requests.
      var entry = {
        rid: memo.id, src: 'memo', newConv: false, at: Date.now(),
        text: 'Voice memo recorded ' + memoTime(memo.at) + ': ' + text,
      };
      var pending = outbox();
      if (!pending.some(function (e) { return e.rid === entry.rid; })) {
        pending.push(entry);
        writeJson('hp2.outbox', pending);
      }
      known[entry.rid] = STATE.QUEUED;
      updateMemo(memo.id, { status: 'sent' });
      lastSubmit = Date.now();
      schedulePoll(0);
      return dropMemoAudio(memo.id);
    });
  }).then(function () {
    memoBusy = false;
    processMemos();
  }, function (e) {
    memoBusy = false;
    console.log('memo failed: ' + ((e && e.message) || e));
  });
}

function onMemo(m) {
  var id = m.rid;
  if (m.op === W.MEMO_BEGIN) {
    if (memoList().some(function (memo) { return memo.id === id; })) {
      send(P.MEMO_ACK, { rid: id, a: 1, b: 0 });      // already have it
      return;
    }
    if (!incoming || incoming.id !== id || incoming.total !== m.a) {
      incoming = {
        id: id, total: m.a, seconds: m.b, crc: m.c >>> 0, at: parseInt(m.txt, 10) || Math.floor(Date.now() / 1000),
        bytes: new Uint8Array(m.a), have: 0,
      };
    }
    send(P.MEMO_ACK, { rid: id, a: 0, b: incoming.have });
  } else if (m.op === W.MEMO_DATA) {
    if (!incoming || incoming.id !== id || m.a !== incoming.have || !m.bin) return;
    var data = m.bin;
    if (incoming.have + data.length > incoming.total) return;
    incoming.bytes.set(data, incoming.have);
    incoming.have += data.length;
  } else if (m.op === W.MEMO_END) {
    var memo = incoming;
    if (!memo || memo.id !== id) {
      send(P.MEMO_ACK, { rid: id, a: 2, b: 0 });
      return;
    }
    if (memo.have !== memo.total || lib.crc32(memo.bytes) !== memo.crc) {
      // Something is missing or damaged: ask for it all again.
      incoming = null;
      send(P.MEMO_ACK, { rid: id, a: 2, b: 0 });
      return;
    }
    saveMemoAudio(id, memo.bytes).then(function () {
      var list = memoList();
      list.push({ id: id, at: memo.at, seconds: memo.seconds, status: 'stored', text: '' });
      writeJson('hp2.memos', list.slice(-20));
      incoming = null;
      send(P.MEMO_ACK, { rid: id, a: 1, b: memo.total });
      processMemos();
    }, function () {
      send(P.TOAST, { txt: 'The phone has no room for the memo' }, 'toast');
    });
  }
}

// ---- notes ---------------------------------------------------------------------------------

var noteRx = null;

function onNote(m) {
  if (!m.bin) return;
  if (!noteRx || noteRx.id !== m.rid || m.a === 0) {
    noteRx = { id: m.rid, total: m.b, crc: m.c >>> 0, bytes: new Uint8Array(m.b), have: 0 };
  }
  if (m.a !== noteRx.have || noteRx.have + m.bin.length > noteRx.total) return;
  noteRx.bytes.set(m.bin, noteRx.have);
  noteRx.have += m.bin.length;
  if (noteRx.have < noteRx.total) return;
  if (lib.crc32(noteRx.bytes) !== noteRx.crc) {
    noteRx = null;                 // the watch still has it and will send again
    return;
  }
  var notes = readJson('hp2.notes', []).filter(function (n) { return n.id !== m.rid; });
  notes.push({ id: m.rid, at: Date.now(), data: lib.base64Encode(noteRx.bytes) });
  writeJson('hp2.notes', notes.slice(-12));
  noteRx = null;
  send(P.NOTE_ACK, { rid: m.rid });
}

// ---- wiring --------------------------------------------------------------------------------

function onModel(m) {
  if (m.a === 1) {
    stt.download();
  } else if (m.a === 2) {
    stt.cancel();
  } else if (m.a === 3 || m.a === 4) {
    rpc('setSpeech', { mode: m.a === 3 ? 'pebble' : 'phone' }).then(function (result) {
      status = result;
      sendState();
    }, function () { sendState(); });
  } else {
    sendState();
  }
}

Pebble.addEventListener('appmessage', function (e) {
  var m = e.payload || {};
  switch (m.op) {
    case W.HELLO:
      known = {};
      refreshStatus().then(function () {
        schedulePoll(0);
        processMemos();
      });
      break;
    case W.LISTEN: onListen(m); break;
    case W.ASK: onAsk(m); break;
    case W.OPEN: onOpen(m); break;
    case W.STOP: onStop(m); break;
    case W.RECENT: onRecent(m); break;
    case W.NEWCONV:
      rpc('newConversation').then(function (result) {
        if (status) status.conv = result.conv;
        sendState();
      }, function () {});
      break;
    case W.MEMO_BEGIN:
    case W.MEMO_DATA:
    case W.MEMO_END: onMemo(m); break;
    case W.NOTE: onNote(m); break;
    case W.MODEL: onModel(m); break;
    case W.PING: send(P.PONG, { a: m.a }); break;
  }
});

// The settings page can reach this side while the watch app is open.
Pebble.addEventListener('configmessage', function (e) {
  var message = e.data || e.message || {};
  if (message.type === 'sttDownload') stt.download();
  if (message.type === 'sttCancel') stt.cancel();
  if (message.type === 'refresh') refreshStatus();
  if (e.respond) e.respond({ stt: stt.summary() });
});

Pebble.addEventListener('ready', function () {
  send(P.READY, {});
});
