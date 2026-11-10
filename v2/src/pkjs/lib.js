// Pure helpers for the phone side: no Pebble, no network. Tested under Node.
//
// Written for the SDK's bundler, which parses ES2015 at most: no async/await here.
'use strict';

// ---- protocol (mirrors src/c/app.h) --------------------------------------------------------

var W = {
  HELLO: 1, LISTEN: 2, ASK: 4, OPEN: 5, STOP: 6, RECENT: 7, NEWCONV: 8,
  MEMO_BEGIN: 9, MEMO_DATA: 10, MEMO_END: 11, NOTE: 12, MODEL: 13, PING: 14,
};
var P = {
  READY: 64, STATE: 65, PARTIAL: 66, FINAL: 67, VAD: 68, STORED: 69, ITEM: 70, PAGE: 71,
  RECENT: 72, MEMO_ACK: 73, NOTE_ACK: 74, PONG: 75, TOAST: 76, DONE: 77,
};
var PF = {
  CONFIGURED: 1, STT_SUPPORT: 2, STT_PHONE: 4, MODEL_READY: 8, DOWNLOADING: 16,
  CONV_ACTIVE: 64, MODEL_ERROR: 128,
};
var STATE = {
  QUEUED: 1, SUBMITTING: 2, ACCEPTED: 3, WORKING: 4, APPROVAL: 5, ANSWERED: 6,
  FAILED: 7, STOPPED: 8, CANCELLED: 9, UNKNOWN: 10, STOPPING: 11,
};
var PAGE_BYTES = 880;
var TEXT_BYTES = 1000;

function isFinal(state) {
  return state === STATE.ANSWERED || state === STATE.FAILED || state === STATE.STOPPED ||
    state === STATE.CANCELLED;
}

// ---- bytes ---------------------------------------------------------------------------------

function utf8Encode(text) {
  var out = [];
  for (var i = 0; i < text.length; i++) {
    var c = text.charCodeAt(i);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.length) {
      var d = text.charCodeAt(i + 1);
      if (d >= 0xDC00 && d <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 63));
    else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
  }
  return out;
}

function utf8Decode(bytes, start, end) {
  var out = '';
  var i = start || 0;
  var stop = end === undefined ? bytes.length : end;
  while (i < stop) {
    var b = bytes[i++];
    var c;
    if (b < 0x80) c = b;
    else if (b < 0xE0) c = ((b & 31) << 6) | (bytes[i++] & 63);
    else if (b < 0xF0) { c = ((b & 15) << 12) | ((bytes[i] & 63) << 6) | (bytes[i + 1] & 63); i += 2; }
    else {
      c = ((b & 7) << 18) | ((bytes[i] & 63) << 12) | ((bytes[i + 1] & 63) << 6) | (bytes[i + 2] & 63);
      i += 3;
    }
    if (c >= 0x10000) {
      c -= 0x10000;
      out += String.fromCharCode(0xD800 + (c >> 10), 0xDC00 + (c & 1023));
    } else {
      out += String.fromCharCode(c);
    }
  }
  return out;
}

/** The text cut to at most `max` UTF-8 bytes, never inside a character. */
function clipBytes(text, max) {
  var bytes = utf8Encode(String(text || ''));
  if (bytes.length <= max) return String(text || '');
  var end = max;
  while (end > 0 && (bytes[end] & 0xC0) === 0x80) end--;
  return utf8Decode(bytes, 0, end);
}

var CRC_TABLE = null;
function crc32(bytes) {
  if (!CRC_TABLE) {
    CRC_TABLE = [];
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      CRC_TABLE.push(c >>> 0);
    }
  }
  var crc = 0xFFFFFFFF;
  for (var i = 0; i < bytes.length; i++) crc = CRC_TABLE[(crc ^ bytes[i]) & 255] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function base64Encode(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i += 3) {
    var a = bytes[i], b = bytes[i + 1], c = bytes[i + 2];
    out += B64[a >> 2] + B64[((a & 3) << 4) | ((b || 0) >> 4)];
    out += i + 1 < bytes.length ? B64[((b & 15) << 2) | ((c || 0) >> 6)] : '=';
    out += i + 2 < bytes.length ? B64[c & 63] : '=';
  }
  return out;
}

function base64Decode(text) {
  var out = [];
  var clean = String(text || '').replace(/[^A-Za-z0-9+/]/g, '');
  for (var i = 0; i < clean.length; i += 4) {
    var a = B64.indexOf(clean[i]), b = B64.indexOf(clean[i + 1]);
    var c = i + 2 < clean.length ? B64.indexOf(clean[i + 2]) : -1;
    var d = i + 3 < clean.length ? B64.indexOf(clean[i + 3]) : -1;
    out.push((a << 2) | (b >> 4));
    if (c >= 0) out.push(((b & 15) << 4) | (c >> 2));
    if (d >= 0) out.push(((c & 3) << 6) | d);
  }
  return out;
}

// ---- answer pages --------------------------------------------------------------------------

function isSpace(b) { return b === 32 || b === 10 || b === 9; }

/**
 * One page of an answer, starting at byte `offset` of its UTF-8 form.
 * Pages end at a paragraph, else a sentence, else a word, and the break itself is dropped, so
 * the watch can lay each page out on its own.
 * Returns { text, next, paragraph } where `next` is the offset of the following page.
 */
function pageAt(bytes, offset, max) {
  var limit = max || PAGE_BYTES;
  var start = offset;
  while (start < bytes.length && isSpace(bytes[start])) start++;
  // A page opens a paragraph when a line break precedes it.
  var paragraph = start === 0;
  for (var back = start - 1; back >= 0 && isSpace(bytes[back]); back--) {
    if (bytes[back] === 10) { paragraph = true; break; }
  }
  if (start >= bytes.length) return { text: '', next: bytes.length, paragraph: paragraph };
  var end = bytes.length;
  if (end - start > limit) {
    var hard = start + limit;
    while (hard > start && (bytes[hard] & 0xC0) === 0x80) hard--;
    var floor = start + Math.floor(limit / 2);
    end = -1;
    var i;
    for (i = hard; i > floor && end < 0; i--) {
      if (bytes[i] === 10 && bytes[i - 1] === 10) end = i - 1;          // paragraph
    }
    for (i = hard; i > floor && end < 0; i--) {
      if (bytes[i] === 10) end = i;                                    // line
    }
    for (i = hard; i > floor && end < 0; i--) {
      if (isSpace(bytes[i]) && (bytes[i - 1] === 46 || bytes[i - 1] === 33 || bytes[i - 1] === 63)) end = i;
    }
    for (i = hard; i > floor && end < 0; i--) {
      if (isSpace(bytes[i])) end = i;                                  // word
    }
    if (end < 0) end = hard;
  }
  var next = end;
  while (next < bytes.length && isSpace(bytes[next])) next++;
  var stop = end;
  while (stop > start && isSpace(bytes[stop - 1])) stop--;
  return { text: utf8Decode(bytes, start, stop), next: next, paragraph: paragraph };
}

// ---- audio ---------------------------------------------------------------------------------

var ADPCM_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8];
var ADPCM_STEPS = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
  544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
  2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];

/** Watch memo audio (4-bit IMA ADPCM, low nibble first, 8 kHz) to 16 kHz float samples. */
function memoToFloat16k(bytes) {
  var out = new Float32Array(bytes.length * 4);
  var predicted = 0, index = 0, previous = 0, at = 0;
  for (var i = 0; i < bytes.length; i++) {
    for (var half = 0; half < 2; half++) {
      var code = half === 0 ? bytes[i] & 15 : bytes[i] >> 4;
      var step = ADPCM_STEPS[index];
      var delta = step >> 3;
      if (code & 4) delta += step;
      if (code & 2) delta += step >> 1;
      if (code & 1) delta += step >> 2;
      predicted += (code & 8) ? -delta : delta;
      predicted = predicted > 32767 ? 32767 : predicted < -32768 ? -32768 : predicted;
      index += ADPCM_INDEX[code & 7];
      index = index < 0 ? 0 : index > 88 ? 88 : index;
      var sample = predicted / 32768;
      // Doubling the rate: the midpoint, then the sample.
      out[at++] = (previous + sample) / 2;
      out[at++] = sample;
      previous = sample;
    }
  }
  return out;
}

/**
 * Tells speech from silence in 16 kHz audio, 20 ms at a time.
 * `ended` turns true once speech has been followed by a long enough pause.
 */
function Vad(options) {
  var o = options || {};
  this.pauseMs = o.pauseMs || 1600;
  this.patienceMs = o.patienceMs || 8000;
  this.floor = 0.004;
  this.speechMs = 0;
  this.silenceMs = 0;
  this.totalMs = 0;
  this.heard = false;
  this.rest = null;
}

Vad.prototype.feed = function (samples) {
  var data = samples;
  if (this.rest && this.rest.length) {
    data = new Float32Array(this.rest.length + samples.length);
    data.set(this.rest);
    data.set(samples, this.rest.length);
  }
  var frame = 320;
  var i = 0;
  for (; i + frame <= data.length; i += frame) {
    var sum = 0;
    for (var j = i; j < i + frame; j++) sum += data[j] * data[j];
    var rms = Math.sqrt(sum / frame);
    this.totalMs += 20;
    var loud = rms > Math.max(this.floor * 3, 0.012);
    if (loud) {
      this.speechMs += 20;
      this.silenceMs = 0;
      if (this.speechMs >= 160) this.heard = true;
    } else {
      this.silenceMs += 20;
      if (this.silenceMs > 200 && !this.heard) this.speechMs = 0;
      // The room's noise floor follows the quiet frames.
      this.floor = this.floor * 0.95 + rms * 0.05;
    }
  }
  this.rest = data.subarray(i);
};

Vad.prototype.ended = function () {
  if (this.heard) return this.silenceMs >= this.pauseMs;
  return this.totalMs >= this.patienceMs;
};

/** Cuts long audio into windows the model accepts, at the quietest moment near each limit. */
function splitForModel(samples, maxSeconds) {
  var max = (maxSeconds || 25) * 16000;
  var parts = [];
  var start = 0;
  while (samples.length - start > max) {
    var from = start + max - 5 * 16000;
    var best = start + max, bestEnergy = Infinity;
    for (var i = from; i + 1600 <= start + max; i += 800) {
      var e = 0;
      for (var j = i; j < i + 1600; j++) e += samples[j] * samples[j];
      if (e < bestEnergy) { bestEnergy = e; best = i + 800; }
    }
    parts.push(samples.subarray(start, best));
    start = best;
  }
  parts.push(samples.subarray(start));
  return parts;
}

function megabytes(bytes) { return Math.round(bytes / 1048576) + ' MB'; }

module.exports = {
  W: W, P: P, PF: PF, STATE: STATE, PAGE_BYTES: PAGE_BYTES, TEXT_BYTES: TEXT_BYTES,
  isFinal: isFinal, utf8Encode: utf8Encode, utf8Decode: utf8Decode, clipBytes: clipBytes,
  crc32: crc32, base64Encode: base64Encode, base64Decode: base64Decode, pageAt: pageAt,
  memoToFloat16k: memoToFloat16k, Vad: Vad, splitForModel: splitForModel, megabytes: megabytes,
};
