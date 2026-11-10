// Hermes plugin: everything the phone does with the Hermes server.
//
// A plugin is started fresh for each request and has no timers, so every entry point here does a
// bounded amount of work and returns. Anything that has to happen later (polling a run that is
// still working) is driven by whoever calls: the watch app's PebbleKit JS, through `watch_rpc`.
//
// State lives in localStorage, credentials in Pebble.secureStorage.

(function () {
  'use strict';

  var CFG_KEY = 'hp2.cfg';
  var CONV_KEY = 'hp2.conv';
  var ITEMS_KEY = 'hp2.items';
  var KEY_API = 'apiKey';
  var KEY_EXTRA = 'extraValue';

  var MAX_ITEMS = 12;          // kept on the phone; the watch lists the newest three
  var MAX_ANSWER = 24000;      // characters of one answer kept for the watch
  var MAX_QUESTION = 2000;

  // Shared with the watch (src/c/proto.h).
  var S = {
    QUEUED: 1, SUBMITTING: 2, ACCEPTED: 3, WORKING: 4, APPROVAL: 5, ANSWERED: 6,
    FAILED: 7, STOPPED: 8, CANCELLED: 9, UNKNOWN: 10, STOPPING: 11,
  };
  var TERMINAL = {};
  TERMINAL[S.ANSWERED] = TERMINAL[S.FAILED] = TERMINAL[S.STOPPED] = TERMINAL[S.CANCELLED] = true;

  var BRIEF = 'You are answering on a small smartwatch screen. Reply in a few short, plain ' +
    'sentences. Do not use Markdown, lists, tables, headings, code blocks or emoji.';

  var DEFAULT_CFG = {
    url: '',
    extraOn: false,
    extraName: '',
    brief: false,
    idleHours: 4,
    sttMode: 'phone',            // 'phone' = on-phone model, 'pebble' = standard dictation
    sttModel: 'moonshine-tiny',
    sttLanguage: '',
    autoStop: true,
  };

  // ---- storage ---------------------------------------------------------------------------

  function readJson(key, fallback) {
    try {
      var raw = localStorage.getItem(key);
      return raw ? JSON.parse(raw) : fallback;
    } catch (e) {
      return fallback;
    }
  }

  function writeJson(key, value) {
    localStorage.setItem(key, JSON.stringify(value));
  }

  function loadCfg() {
    var stored = readJson(CFG_KEY, {});
    var cfg = {};
    Object.keys(DEFAULT_CFG).forEach(function (k) {
      cfg[k] = stored[k] === undefined ? DEFAULT_CFG[k] : stored[k];
    });
    return cfg;
  }

  function loadItems() { return readJson(ITEMS_KEY, []); }

  function saveItems(items) {
    // Newest first. Items still in flight are never dropped.
    items.sort(function (x, y) { return y.createdAt - x.createdAt; });
    var kept = [];
    items.forEach(function (item) {
      if (kept.length < MAX_ITEMS || !TERMINAL[item.state]) kept.push(item);
    });
    writeJson(ITEMS_KEY, kept);
  }

  function loadConv() { return readJson(CONV_KEY, { gen: 1, sessionId: '', lastAt: 0, turns: 0 }); }

  function freshConv(conv) {
    return { gen: (conv.gen || 0) + 1, sessionId: '', lastAt: 0, turns: 0 };
  }

  // ---- text for a watch --------------------------------------------------------------------

  var PUNCT = {
    '‘': "'", '’': "'", '‚': "'", '“': '"', '”': '"', '„': '"',
    '–': '-', '—': '-', '−': '-', '…': '...', ' ': ' ', '•': '•',
    '→': '->', '←': '<-', '×': 'x', '✓': 'ok', '✔': 'ok',
  };

  /** Markdown and symbols the watch fonts cannot draw, reduced to plain readable text. */
  function watchText(text) {
    var out = String(text || '').replace(/\r\n?/g, '\n');
    // Fenced code keeps its content, loses its fence.
    out = out.replace(/```[a-zA-Z0-9_-]*\n?/g, '').replace(/```/g, '');
    out = out.replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1');
    out = out.replace(/\[([^\]]+)\]\([^)]*\)/g, '$1');
    out = out.split('\n').map(function (line) {
      var l = line.replace(/^\s{0,3}#{1,6}\s+/, '');
      l = l.replace(/^\s{0,3}>\s?/, '');
      if (/^\s*\|?[\s:|-]+\|[\s:|-]*$/.test(l)) return null;                // table rule
      if (/^\s*\|.*\|\s*$/.test(l)) {
        l = l.trim().replace(/^\|/, '').replace(/\|$/, '').split('|')
          .map(function (c) { return c.trim(); }).join(' · ');
      }
      l = l.replace(/^(\s*)[-*+]\s+/, '$1• ');
      if (/^\s*([-*_])\s*(\1\s*){2,}$/.test(l)) return '';                   // horizontal rule
      return l;
    }).filter(function (l) { return l !== null; }).join('\n');
    out = out.replace(/(\*\*|__)(.*?)\1/g, '$2').replace(/(^|[^\w*])[*_]([^*_\n]+)[*_](?=[^\w*]|$)/g, '$1$2');
    out = out.replace(/`([^`]*)`/g, '$1');
    out = out.replace(/[‘’‚“”„–—−… →←×✓✔]/g,
      function (c) { return PUNCT[c]; });
    // Keep what Pebble's system fonts cover: Latin, Latin-1, Latin Extended-A and the bullet.
    out = out.replace(/[^\n\x20-\x7E¡-ſ•]/g, '');
    out = out.replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').replace(/[ \t]{2,}/g, ' ');
    return out.trim();
  }

  // ---- HTTP ------------------------------------------------------------------------------

  /** The API root as typed, without a trailing slash or a trailing /v1. */
  function normalizeUrl(url) {
    var u = String(url || '').trim().replace(/\/+$/, '');
    u = u.replace(/\/v1$/i, '');
    return u;
  }

  function hostOf(url) {
    var m = /^[a-z]+:\/\/([^/:?#]+)/i.exec(url || '');
    return m ? m[1].toLowerCase() : '';
  }

  function urlProblem(url) {
    if (!url) return 'No server address yet';
    var host = hostOf(url);
    if (!host) return 'The server address is not a URL';
    if (/^https:\/\//i.test(url)) return '';
    var local = host === 'localhost' || host === '127.0.0.1' || host === '10.0.2.2';
    if (/^http:\/\//i.test(url) && local) return '';
    return 'The server address must start with https://';
  }

  function Problem(kind, message, status) {
    this.kind = kind;          // 'config' | 'permission' | 'network' | 'auth' | 'http'
    this.message = message;
    this.status = status || 0;
  }

  async function credentials(cfg) {
    var headers = {};
    var key = await Pebble.secureStorage.getItem(KEY_API);
    if (key) headers.Authorization = 'Bearer ' + key;
    if (cfg.extraOn && cfg.extraName) {
      var extra = await Pebble.secureStorage.getItem(KEY_EXTRA);
      if (extra) headers[cfg.extraName] = extra;
    }
    return headers;
  }

  /** One request to Hermes. Resolves to { status, body } or throws a Problem. */
  async function call(cfg, method, path, body, extraHeaders) {
    var base = normalizeUrl(cfg.url);
    var problem = urlProblem(base);
    if (problem) throw new Problem('config', problem);
    var headers = await credentials(cfg);
    headers.Accept = 'application/json';
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    Object.keys(extraHeaders || {}).forEach(function (k) { headers[k] = extraHeaders[k]; });

    var response;
    try {
      response = await fetch(base + path, {
        method: method,
        headers: headers,
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch (e) {
      if (e && e.name === 'NotAllowedError') {
        throw new Problem('permission', 'Allow ' + hostOf(base) + ' for Hermes in the Pebble app');
      }
      throw new Problem('network', 'Phone cannot reach Hermes');
    }
    var text = '';
    try { text = await response.text(); } catch (e) { /* empty body */ }
    var parsed = null;
    try { parsed = text ? JSON.parse(text) : null; } catch (e) { /* not JSON */ }
    return { status: response.status, body: parsed, text: text };
  }

  function errorText(reply) {
    var b = reply.body;
    var detail = '';
    if (b && typeof b === 'object') {
      var e = b.error;
      if (typeof e === 'string') detail = e;
      else if (e && typeof e === 'object') detail = e.message || e.code || '';
      if (!detail) detail = b.message || b.detail || '';
    }
    return String(detail || '').slice(0, 120);
  }

  // ---- runs ------------------------------------------------------------------------------

  /** The answer text from a run object, whichever shape the server used for it. */
  function outputText(run) {
    var out = run.output;
    if (typeof out === 'string') return out;
    if (out && typeof out === 'object' && !Array.isArray(out)) {
      if (typeof out.text === 'string') return out.text;
      if (typeof out.content === 'string') return out.content;
      out = out.content || out.output || [];
    }
    if (!Array.isArray(out)) return '';
    var parts = [];
    out.forEach(function (item) {
      if (typeof item === 'string') { parts.push(item); return; }
      if (!item || (item.type && item.type !== 'message' && item.type !== 'output_text' && item.type !== 'text')) return;
      if (typeof item.text === 'string') { parts.push(item.text); return; }
      var content = item.content;
      if (typeof content === 'string') { parts.push(content); return; }
      (content || []).forEach(function (part) {
        if (part && typeof part.text === 'string') parts.push(part.text);
      });
    });
    return parts.join('\n\n');
  }

  function runError(run) {
    var e = run.error;
    if (!e) return '';
    if (typeof e === 'string') return e.slice(0, 160);
    return String(e.message || e.code || 'Hermes reported an error').slice(0, 160);
  }

  function summary(item) {
    return {
      rid: item.rid,
      state: item.state,
      q: item.q,
      src: item.src,
      err: item.err || '',
      bytes: utf8Length(item.answer || ''),
      createdAt: item.createdAt,
      updatedAt: item.updatedAt,
    };
  }

  function utf8Length(text) {
    var n = 0;
    for (var i = 0; i < text.length; i++) {
      var c = text.charCodeAt(i);
      n += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    }
    return n;
  }

  function touch(item, state, err) {
    item.state = state;
    item.err = err || '';
    item.updatedAt = Date.now();
  }

  /** True when an earlier turn of the same conversation has not told us its session yet. */
  function waitsForSession(items, item, conv) {
    if (conv.sessionId || item.gen !== conv.gen) return false;
    return items.some(function (other) {
      return other !== item && other.gen === item.gen && other.createdAt <= item.createdAt &&
        !TERMINAL[other.state] && other.state !== S.QUEUED;
    });
  }

  async function submitRun(cfg, items, item) {
    var conv = loadConv();
    if (waitsForSession(items, item, conv)) return;
    var body = { input: item.q };
    if (item.gen === conv.gen && conv.sessionId) body.session_id = conv.sessionId;
    if (cfg.brief) body.instructions = BRIEF;

    touch(item, S.SUBMITTING);
    item.attempts = (item.attempts || 0) + 1;
    var reply;
    try {
      // The conversation is part of the key: after Hermes refused a session (404) the retry is a
      // different request and must not be matched against the refused one.
      reply = await call(cfg, 'POST', '/v1/runs', body, {
        'Idempotency-Key': 'hp2-' + item.rid + '-' + item.createdAt.toString(36) + '-' + item.gen,
      });
    } catch (problem) {
      // Nothing reached Hermes for certain; the same key makes the retry safe either way.
      touch(item, S.QUEUED, problem.message);
      return;
    }
    if (reply.status >= 200 && reply.status < 300 && reply.body && reply.body.run_id) {
      item.runId = reply.body.run_id;
      touch(item, S.ACCEPTED);
      return;
    }
    if (reply.status === 401 || reply.status === 403) {
      touch(item, S.QUEUED, 'Hermes rejected the API key');
    } else if (reply.status === 429 || reply.status === 503 || reply.status === 502 || reply.status === 504) {
      touch(item, S.QUEUED, 'Hermes is busy, retrying');
    } else if (reply.status === 409) {
      touch(item, S.UNKNOWN, errorText(reply) || 'Hermes saw this request differently before');
    } else if (reply.status === 404 && body.session_id) {
      // The conversation is gone on the server: carry on in a new one.
      writeJson(CONV_KEY, freshConv(conv));
      item.gen = loadConv().gen;
      touch(item, S.QUEUED, 'Started a new conversation');
    } else {
      touch(item, S.FAILED, errorText(reply) || ('Hermes answered HTTP ' + reply.status));
    }
  }

  function learnSession(item, run) {
    if (!run.session_id) return;
    item.sessionId = run.session_id;
    var conv = loadConv();
    if (item.gen === conv.gen && conv.sessionId !== run.session_id) {
      conv.sessionId = run.session_id;
      writeJson(CONV_KEY, conv);
    }
  }

  async function refreshRun(cfg, item) {
    var reply;
    try {
      reply = await call(cfg, 'GET', '/v1/runs/' + encodeURIComponent(item.runId));
    } catch (problem) {
      item.err = problem.message;      // state unchanged: the run carries on without us
      return;
    }
    if (reply.status === 404) {
      touch(item, S.UNKNOWN, 'Hermes no longer knows this request');
      return;
    }
    if (reply.status < 200 || reply.status >= 300 || !reply.body) {
      item.err = reply.status === 401 || reply.status === 403
        ? 'Hermes rejected the API key' : 'Hermes answered HTTP ' + reply.status;
      return;
    }
    var run = reply.body;
    learnSession(item, run);
    var status = String(run.status || '').toLowerCase();
    if (status === 'completed') {
      item.answer = watchText(outputText(run)).slice(0, MAX_ANSWER) || '(Hermes finished without a reply.)';
      touch(item, S.ANSWERED);
      var conv = loadConv();
      if (item.gen === conv.gen) { conv.lastAt = Date.now(); writeJson(CONV_KEY, conv); }
    } else if (status === 'failed' || status === 'interrupted') {
      touch(item, S.FAILED, runError(run) || (status === 'interrupted' ? 'Hermes was interrupted' : 'Hermes could not finish'));
    } else if (status === 'cancelled') {
      touch(item, item.stopAsked ? S.STOPPED : S.CANCELLED);
    } else if (status === 'waiting_for_approval') {
      touch(item, S.APPROVAL);
    } else if (status === 'stopping') {
      touch(item, S.STOPPING);
    } else if (status === 'running') {
      touch(item, item.stopAsked ? S.STOPPING : S.WORKING);
    } else if (status === 'queued' || status === 'started') {
      touch(item, S.ACCEPTED);
    } else {
      touch(item, S.UNKNOWN, status ? 'Hermes reports "' + status + '"' : '');
    }
  }

  /** Move one item as far as a single request can take it. */
  async function advance(cfg, items, item) {
    if (TERMINAL[item.state]) return;
    if (item.state === S.QUEUED || item.state === S.SUBMITTING) {
      await submitRun(cfg, items, item);
      if (item.state !== S.ACCEPTED) return;
    }
    if (item.runId) await refreshRun(cfg, item);
  }

  function newRid() {
    var n = Math.floor(Math.random() * 0x7fffffff) + 1;
    return n;
  }

  function applyIdle(cfg, conv) {
    var idleMs = (Number(cfg.idleHours) || 0) * 3600000;
    if (idleMs > 0 && conv.lastAt && Date.now() - conv.lastAt > idleMs) return freshConv(conv);
    return conv;
  }

  function createItem(items, params) {
    var cfg = loadCfg();
    var conv = applyIdle(cfg, loadConv());
    if (params.newConv) conv = freshConv(conv);
    conv.lastAt = Date.now();
    conv.turns = (conv.turns || 0) + 1;
    writeJson(CONV_KEY, conv);
    var now = Date.now();
    var item = {
      rid: params.rid,
      q: String(params.text || '').trim().slice(0, MAX_QUESTION),
      src: params.src || 'watch',
      gen: conv.gen,
      state: S.QUEUED,
      err: '',
      runId: '',
      answer: '',
      createdAt: now,
      updatedAt: now,
    };
    items.push(item);
    return item;
  }

  // ---- watch_rpc ---------------------------------------------------------------------------

  function convStatus(cfg) {
    var stored = loadConv();
    var conv = applyIdle(cfg, stored);
    if (conv !== stored) writeJson(CONV_KEY, conv);
    return {
      active: (conv.turns || 0) > 0,
      turns: conv.turns || 0,
      idleMin: conv.lastAt ? Math.floor((Date.now() - conv.lastAt) / 60000) : -1,
      gen: conv.gen,
    };
  }

  async function publicStatus() {
    var cfg = loadCfg();
    var key = await Pebble.secureStorage.getItem(KEY_API);
    var base = normalizeUrl(cfg.url);
    return {
      configured: !urlProblem(base) && !!key,
      host: hostOf(base),
      conv: convStatus(cfg),
      stt: { mode: cfg.sttMode, model: cfg.sttModel, language: cfg.sttLanguage, autoStop: !!cfg.autoStop },
    };
  }

  var rpc = {
    status: function () { return publicStatus(); },

    /** Store a question and start it. Safe to repeat: the rid identifies the request. */
    submit: async function (p) {
      var items = loadItems();
      var item = items.filter(function (i) { return i.rid === p.rid; })[0];
      var cfg = loadCfg();
      if (!item) {
        if (!String(p.text || '').trim()) throw new Problem('config', 'Nothing to send');
        item = createItem(items, p);
        saveItems(items);                 // durable before any network
      }
      await advance(cfg, items, item);
      saveItems(items);
      return summary(item);
    },

    /** Advance every unfinished item by one step and report them. */
    poll: async function () {
      var items = loadItems();
      var cfg = loadCfg();
      var open = items.filter(function (i) { return !TERMINAL[i.state]; });
      // Oldest first, so a conversation's turns are submitted in order.
      open.sort(function (x, y) { return x.createdAt - y.createdAt; });
      for (var n = 0; n < open.length; n++) await advance(cfg, items, open[n]);
      if (open.length) saveItems(items);
      return { items: open.map(summary), conv: convStatus(cfg) };
    },

    get: function (p) {
      var item = loadItems().filter(function (i) { return i.rid === p.rid; })[0];
      if (!item) return { missing: true, rid: p.rid };
      var result = summary(item);
      if (p.withText) result.answer = item.answer || '';
      return result;
    },

    recent: function (p) {
      var items = loadItems();
      items.sort(function (x, y) { return y.createdAt - x.createdAt; });
      return { items: items.slice(0, (p && p.n) || 3).map(summary) };
    },

    stop: async function (p) {
      var items = loadItems();
      var item = items.filter(function (i) { return i.rid === p.rid; })[0];
      if (!item) return { missing: true, rid: p.rid };
      if (!TERMINAL[item.state]) {
        if (!item.runId) {
          touch(item, S.CANCELLED);
        } else {
          item.stopAsked = true;
          try {
            var reply = await call(loadCfg(), 'POST', '/v1/runs/' + encodeURIComponent(item.runId) + '/stop');
            if (reply.status >= 200 && reply.status < 300) touch(item, S.STOPPING);
            else item.err = errorText(reply) || 'Hermes would not stop it (HTTP ' + reply.status + ')';
          } catch (problem) {
            item.err = problem.message;
          }
        }
        saveItems(items);
      }
      return summary(item);
    },

    newConversation: function () {
      writeJson(CONV_KEY, freshConv(loadConv()));
      return { conv: convStatus(loadCfg()) };
    },

    /** The one preference the watch can change itself: where speech is transcribed. */
    setSpeech: function (p) {
      var cfg = loadCfg();
      cfg.sttMode = p.mode === 'pebble' ? 'pebble' : 'phone';
      writeJson(CFG_KEY, cfg);
      return publicStatus();
    },
  };

  // ---- settings page -----------------------------------------------------------------------

  async function testConnection() {
    var cfg = loadCfg();
    var health;
    try {
      health = await call(cfg, 'GET', '/health');
    } catch (problem) {
      return { ok: false, message: problem.message };
    }
    if (health.status < 200 || health.status >= 300) {
      return { ok: false, message: 'No Hermes API at this address (HTTP ' + health.status + ')' };
    }
    var models;
    try {
      models = await call(cfg, 'GET', '/v1/models');
    } catch (problem) {
      return { ok: false, message: problem.message };
    }
    if (models.status === 401 || models.status === 403) {
      return { ok: false, message: 'Hermes is there, but rejected the API key' };
    }
    if (models.status < 200 || models.status >= 300) {
      return { ok: false, message: 'Hermes is there, but answered HTTP ' + models.status };
    }
    var list = (models.body && models.body.data) || [];
    var name = list.length && list[0].id ? list[0].id : 'Hermes';
    return { ok: true, message: 'Connected to ' + name };
  }

  async function settingsSnapshot() {
    var cfg = loadCfg();
    var items = loadItems();
    items.sort(function (x, y) { return y.createdAt - x.createdAt; });
    return {
      cfg: cfg,
      hasKey: !!(await Pebble.secureStorage.getItem(KEY_API)),
      hasExtra: !!(await Pebble.secureStorage.getItem(KEY_EXTRA)),
      conv: convStatus(cfg),
      items: items.map(function (i) {
        var s = summary(i);
        s.preview = (i.answer || '').slice(0, 280);
        return s;
      }),
      stt: readJson('hp2.stt', null),
      notes: readJson('hp2.notes', []),
      memos: readJson('hp2.memos', []),
    };
  }

  var settings = {
    load: function () { return settingsSnapshot(); },

    save: async function (m) {
      var cfg = loadCfg();
      var next = m.cfg || {};
      Object.keys(DEFAULT_CFG).forEach(function (k) {
        if (next[k] !== undefined) cfg[k] = next[k];
      });
      cfg.url = normalizeUrl(cfg.url);
      cfg.extraName = String(cfg.extraName || '').trim();
      cfg.idleHours = Number(cfg.idleHours) || 0;
      writeJson(CFG_KEY, cfg);
      // Secrets only ever travel towards storage. An empty string leaves the saved one alone.
      if (typeof m.apiKey === 'string' && m.apiKey) await Pebble.secureStorage.setItem(KEY_API, m.apiKey);
      if (m.clearApiKey) await Pebble.secureStorage.removeItem(KEY_API);
      if (typeof m.extraValue === 'string' && m.extraValue) await Pebble.secureStorage.setItem(KEY_EXTRA, m.extraValue);
      if (m.clearExtra) await Pebble.secureStorage.removeItem(KEY_EXTRA);
      if (Pebble.refreshSources) Pebble.refreshSources(['agent/configured']);
      return settingsSnapshot();
    },

    test: function () { return testConnection(); },

    newConversation: function () {
      rpc.newConversation();
      return settingsSnapshot();
    },

    /** Finished items only: anything still in flight stays so its answer is not lost. */
    clearHistory: function () {
      saveItems(loadItems().filter(function (i) { return !TERMINAL[i.state]; }));
      return settingsSnapshot();
    },

    clearNotes: function () {
      localStorage.removeItem('hp2.notes');
      return settingsSnapshot();
    },
  };

  // ---- handlers ----------------------------------------------------------------------------

  Pebble.registerActionHandler(async function (request, respond) {
    var args = request.args || {};
    try {
      if (request.action === 'watch_rpc') {
        var method = rpc[args.method];
        if (!method) return respond.error('INVALID_ARGS', 'unknown method');
        var result = await method(args.params || {});
        return respond.ok({ text: JSON.stringify(result) });
      }
      if (request.action === 'send_to_agent') {
        var message = String(args.message || '').trim();
        if (!message) return respond.error('INVALID_ARGS', 'message is empty');
        var status = await publicStatus();
        if (!status.configured) return respond.error('AUTH_REQUIRED', 'Set up Hermes in its settings first');
        var item = await rpc.submit({
          rid: newRid(), text: message, src: 'index', newConv: args.chat_id === 'new',
        });
        if (item.state === S.FAILED) return respond.error('UNKNOWN', item.err || 'Hermes refused it');
        return respond.ok({
          text: item.state === S.QUEUED ? 'Saved for Hermes. It will send from the watch app.'
            : 'Sent to Hermes. Read the reply in the Hermes watch app.',
        });
      }
      return respond.error('INVALID_REQUEST', 'unknown action');
    } catch (e) {
      if (e instanceof Problem) return respond.error(e.kind === 'config' ? 'AUTH_REQUIRED' : 'UNKNOWN', e.message);
      return respond.error('UNKNOWN', String((e && e.message) || e));
    }
  });

  Pebble.registerSourceHandler(async function (request, respond) {
    if (request.category !== 'agent') return respond.error('PLUGIN_UNAVAILABLE');
    if (request.item === 'configured') {
      var status = await publicStatus();
      return respond.data({
        validUntilMs: Date.now() + 60000,
        instances: [{
          instanceId: 'hermes',
          properties: {
            name: { shortText: { text: 'Hermes' } },
            configured: {
              boolean: { value: status.configured },
              shortText: { text: status.configured ? 'Ready' : 'Set up' },
            },
          },
        }],
      });
    }
    if (request.item === 'chats') {
      return respond.data({
        validUntilMs: Date.now() + 300000,
        instances: [
          { instanceId: 'current', properties: { name: { shortText: { text: 'Current' } }, title: { shortText: { text: 'Current' }, longText: { text: 'Continue the current conversation' } } } },
          { instanceId: 'new', properties: { name: { shortText: { text: 'New' } }, title: { shortText: { text: 'New' }, longText: { text: 'Start a new conversation' } } } },
        ],
      });
    }
    return respond.error('PLUGIN_UNAVAILABLE');
  });

  Pebble.registerConfigHandler(function (message, respond) {
    var handler = message && settings[message.type];
    if (!handler) { respond({ error: 'unknown message' }); return; }
    Promise.resolve().then(function () { return handler(message); }).then(respond, function (e) {
      respond({ error: String((e && e.message) || e) });
    });
  });

  // For tests.
  if (typeof globalThis !== 'undefined') {
    globalThis.__hermesPlugin = { watchText: watchText, normalizeUrl: normalizeUrl, outputText: outputText, S: S };
  }
})();
