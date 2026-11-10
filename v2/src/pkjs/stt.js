// On-phone speech recognition for PebbleKit JS.
//
// The engine (tools/stt) is a string in stt_engine.json, evalled on first use. Its WebAssembly
// runtime and the chosen model are downloaded once, checked against the hashes pinned at build
// time, and kept in IndexedDB. After that nothing spoken leaves the phone.
//
// Needs WebAssembly and IndexedDB, which the Pebble Android app's JS runtime has. Where they are
// missing, supported() is false and the watch uses Pebble dictation.
'use strict';

var catalog = require('./stt_catalog.json');

var DB_NAME = 'hermes2-stt';
var STORE = 'files';

function supported() {
  try {
    return typeof WebAssembly === 'object' && typeof indexedDB !== 'undefined' && !!indexedDB &&
      typeof XMLHttpRequest !== 'undefined';
  } catch (e) {
    return false;
  }
}

// ---- IndexedDB -----------------------------------------------------------------------------

var dbPromise = null;

function db() {
  if (!dbPromise) {
    dbPromise = new Promise(function (resolve, reject) {
      var request = indexedDB.open(DB_NAME, 1);
      request.onupgradeneeded = function () { request.result.createObjectStore(STORE); };
      request.onsuccess = function () { resolve(request.result); };
      request.onerror = function () { dbPromise = null; reject(request.error || new Error('storage unavailable')); };
    });
  }
  return dbPromise;
}

function store(mode, work) {
  return db().then(function (database) {
    return new Promise(function (resolve, reject) {
      var tx = database.transaction(STORE, mode);
      var request = work(tx.objectStore(STORE));
      tx.oncomplete = function () { resolve(request ? request.result : undefined); };
      tx.onerror = tx.onabort = function () { reject(tx.error || new Error('storage error')); };
    });
  });
}

function idbGet(key) { return store('readonly', function (s) { return s.get(key); }); }
function idbPut(key, value) { return store('readwrite', function (s) { return s.put(value, key); }); }
function idbDelete(key) { return store('readwrite', function (s) { return s.delete(key); }); }
function idbKeys() { return store('readonly', function (s) { return s.getAllKeys(); }); }
function idbHas(key) {
  return store('readonly', function (s) { return s.count(key); }).then(function (n) { return n > 0; });
}

// ---- SHA-256 -------------------------------------------------------------------------------

function hex(buffer) {
  var bytes = new Uint8Array(buffer);
  var out = '';
  for (var i = 0; i < bytes.length; i++) out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
  return out;
}

var K = null;
function sha256Js(buffer) {
  if (!K) {
    K = [];
    for (var candidate = 2; K.length < 64; candidate++) {
      var prime = true;
      for (var f = 2; f * f <= candidate; f++) if (candidate % f === 0) { prime = false; break; }
      if (prime) K.push((Math.pow(candidate, 1 / 3) % 1) * 4294967296 | 0);
    }
  }
  var data = new Uint8Array(buffer);
  var length = data.length;
  var padded = new Uint8Array(((length + 9 + 63) >> 6) << 6);
  padded.set(data);
  padded[length] = 0x80;
  var view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, Math.floor(length / 536870912));
  view.setUint32(padded.length - 4, (length << 3) >>> 0);
  var h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
  var w = new Int32Array(64);
  for (var block = 0; block < padded.length; block += 64) {
    var i;
    for (i = 0; i < 16; i++) w[i] = view.getInt32(block + i * 4);
    for (i = 16; i < 64; i++) {
      var x = w[i - 15], y = w[i - 2];
      var s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      var s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f2 = h[5], g = h[6], hh = h[7];
    for (i = 0; i < 64; i++) {
      var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      var t1 = (hh + S1 + ((e & f2) ^ (~e & g)) + K[i] + w[i]) | 0;
      var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      var t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      hh = g; g = f2; f2 = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
    h[4] = (h[4] + e) | 0; h[5] = (h[5] + f2) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
  }
  var out = new DataView(new ArrayBuffer(32));
  for (var n = 0; n < 8; n++) out.setInt32(n * 4, h[n]);
  return hex(out.buffer);
}

function sha256(buffer) {
  try {
    if (typeof crypto !== 'undefined' && crypto.subtle && crypto.subtle.digest) {
      return crypto.subtle.digest('SHA-256', buffer).then(hex, function () { return sha256Js(buffer); });
    }
  } catch (e) { /* fall through */ }
  return Promise.resolve(sha256Js(buffer));
}

// ---- the manager ---------------------------------------------------------------------------

function Stt(onChange) {
  this.onChange = onChange || function () {};
  this.model = catalog.models[0];
  this.language = '';
  this.status = 'missing';        // missing | downloading | ready | error
  this.percent = 0;
  this.error = '';
  this.xhr = null;
  this.cancelled = false;
  this.loading = null;
  this.loadedKey = '';
  this.chain = Promise.resolve();
  this.engine = null;
}

Stt.supported = supported;
Stt.sha256Js = sha256Js;
Stt.catalog = catalog;

Stt.prototype.runtime = function () {
  if (!this.engine) this.engine = require('./stt_engine.json');
  return this.engine.runtime;
};

Stt.prototype.wanted = function () {
  var model = this.model;
  var runtime = this.runtime();
  var files = [{ key: 'rt/' + runtime.sha256, url: runtime.url, size: runtime.size, sha256: runtime.sha256 }];
  model.files.forEach(function (f) {
    files.push({
      key: 'm/' + model.repo + '@' + model.revision + '/' + f.path,
      url: 'https://huggingface.co/' + model.repo + '/resolve/' + model.revision + '/' + f.path,
      size: f.size,
      sha256: f.sha256,
    });
  });
  return files;
};

Stt.prototype.totalBytes = function () {
  return this.wanted().reduce(function (sum, f) { return sum + f.size; }, 0);
};

Stt.prototype.set = function (status, percent, error) {
  this.status = status;
  this.percent = percent || 0;
  this.error = error || '';
  this.onChange();
};

/** Picks the model and works out whether it is already on the phone. */
Stt.prototype.configure = function (modelId, language) {
  var self = this;
  var model = catalog.models.filter(function (m) { return m.id === modelId; })[0] || catalog.models[0];
  var changed = model !== this.model;
  this.model = model;
  this.language = language || '';
  if (!supported()) return Promise.resolve();
  if (this.status === 'downloading' && !changed) return Promise.resolve();
  if (changed && this.status === 'downloading') this.cancel();
  return Promise.all(this.wanted().map(function (f) { return idbHas(f.key); })).then(function (have) {
    var all = have.every(function (h) { return h; });
    if (self.model === model) self.set(all ? 'ready' : 'missing', 0, '');
  }, function (e) {
    self.set('error', 0, 'Phone storage is not available');
  });
};

Stt.prototype.fetchFile = function (file, onBytes) {
  var self = this;
  return new Promise(function (resolve, reject) {
    var xhr = new XMLHttpRequest();
    self.xhr = xhr;
    xhr.open('GET', file.url);
    xhr.responseType = 'arraybuffer';
    xhr.onprogress = function (e) { onBytes(e.loaded || 0); };
    xhr.onload = function () {
      self.xhr = null;
      if (xhr.status === 403) return reject(new Error('Allow the download for Hermes 2 in the Pebble app'));
      if (xhr.status < 200 || xhr.status >= 300 || !xhr.response) return reject(new Error('Download failed (HTTP ' + xhr.status + ')'));
      resolve(xhr.response);
    };
    xhr.onerror = function () { self.xhr = null; reject(new Error('Download failed. Check the connection and that Hermes 2 may use the internet.')); };
    xhr.onabort = function () { self.xhr = null; reject(new Error('cancelled')); };
    xhr.send();
  });
};

/** Downloads whatever is missing. Files already stored are kept, so a retry resumes. */
Stt.prototype.download = function () {
  var self = this;
  if (!supported() || this.status === 'downloading') return Promise.resolve();
  var model = this.model;
  var files = this.wanted();
  var total = this.totalBytes();
  var done = 0;
  this.cancelled = false;
  this.set('downloading', 0, '');

  function next(index) {
    if (self.cancelled) return Promise.reject(new Error('cancelled'));
    if (index >= files.length) return Promise.resolve();
    var file = files[index];
    return idbHas(file.key).then(function (have) {
      if (have) return null;
      var last = -1;
      return self.fetchFile(file, function (loaded) {
        var percent = Math.min(99, Math.floor((done + loaded) * 100 / total));
        if (percent !== last) { last = percent; self.set('downloading', percent, ''); }
      }).then(function (buffer) {
        if (buffer.byteLength !== file.size) throw new Error('A downloaded file has the wrong size');
        return sha256(buffer).then(function (digest) {
          if (digest !== file.sha256) throw new Error('A downloaded file failed its integrity check');
          return idbPut(file.key, buffer);
        });
      });
    }).then(function () {
      done += file.size;
      return next(index + 1);
    });
  }

  return next(0).then(function () {
    if (self.model !== model) return self.configure(self.model.id, self.language);
    self.set('ready', 100, '');
    // Earlier models and runtimes are of no use now.
    var keep = {};
    files.forEach(function (f) { keep[f.key] = true; });
    return idbKeys().then(function (keys) {
      return Promise.all(keys.filter(function (k) {
        return typeof k === 'string' && (k.indexOf('rt/') === 0 || k.indexOf('m/') === 0) && !keep[k];
      }).map(idbDelete));
    }).catch(function () {});
  }, function (e) {
    if (self.cancelled) self.set('missing', 0, '');
    else self.set('error', 0, String((e && e.message) || e));
  });
};

Stt.prototype.cancel = function () {
  this.cancelled = true;
  if (this.xhr) { try { this.xhr.abort(); } catch (e) { /* gone */ } }
};

Stt.prototype.usable = function () { return supported() && this.status === 'ready'; };
Stt.prototype.isLoaded = function () { return !!this.loadedKey && this.loadedKey === this.model.id; };

/** Loads the engine and the model into memory. Safe to call often. */
Stt.prototype.ensureLoaded = function () {
  var self = this;
  if (!this.usable()) return Promise.reject(new Error('speech model is not ready'));
  if (this.isLoaded()) return Promise.resolve();
  if (this.loading) return this.loading;
  var model = this.model;
  var files = this.wanted();
  var prefix = 'm/' + model.repo + '@' + model.revision + '/';
  this.loading = Promise.resolve().then(function () {
    if (typeof HermesSTT === 'undefined') (0, eval)(self.engine.code);
    return idbGet(files[0].key);
  }).then(function (wasmBinary) {
    if (!wasmBinary) throw new Error('speech runtime is missing');
    return HermesSTT.load({
      wasmBinary: wasmBinary,
      repo: model.repo,
      dtype: model.dtype,
      readFile: function (path) { return idbGet(prefix + path).then(function (b) { return b || null; }); },
    });
  }).then(function () {
    self.loading = null;
    self.loadedKey = model.id;
  }, function (e) {
    self.loading = null;
    self.loadedKey = '';
    throw e;
  });
  return this.loading;
};

/** Transcribes 16 kHz float samples. Calls run one after another. */
Stt.prototype.transcribe = function (samples) {
  var self = this;
  var run = function () {
    return self.ensureLoaded().then(function () {
      return HermesSTT.transcribe(samples, {
        multilingual: !!self.model.multilingual,
        language: self.language || undefined,
        maxNewTokens: 32 + Math.ceil(samples.length / 16000 * 12),
      });
    });
  };
  var result = this.chain.then(run, run);
  this.chain = result.catch(function () {});
  return result;
};

Stt.prototype.summary = function () {
  return {
    supported: supported(),
    model: this.model.id,
    name: this.model.name,
    bytes: supported() ? this.totalBytes() : 0,
    status: this.status,
    percent: this.percent,
    error: this.error,
  };
};

Stt.idb = { get: idbGet, put: idbPut, del: idbDelete };

module.exports = Stt;
