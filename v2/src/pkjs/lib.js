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

/** Watch memo audio (4-bit IMA ADPCM, low nibble first) to 8 kHz float samples, as coded. */
function memoDecode8k(bytes) {
  var out = new Float32Array(bytes.length * 2);
  var predicted = 0, index = 0, at = 0;
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
      out[at++] = predicted / 32768;
    }
  }
  return out;
}

/** Twice the sample rate: the midpoint, then the sample. */
function upsample2x(samples) {
  var out = new Float32Array(samples.length * 2);
  var previous = 0;
  for (var i = 0; i < samples.length; i++) {
    out[2 * i] = (previous + samples[i]) / 2;
    out[2 * i + 1] = samples[i];
    previous = samples[i];
  }
  return out;
}

/** Watch memo audio to 16 kHz float samples, exactly as coded (no cleaning). */
function memoToFloat16k(bytes) { return upsample2x(memoDecode8k(bytes)); }

/**
 * Linear resampling for a stream delivered in pieces. feed() returns the new output samples.
 * Used when the phone delivers the microphone at another rate than the model's 16 kHz.
 */
function Resampler(from, to) {
  this.ratio = from / to;
  this.pos = 0;              // position of the next output sample, in input samples from `last`
  this.last = 0;
}

Resampler.prototype.feed = function (input) {
  if (this.ratio === 1) return input;
  var out = [];
  // Input index -1 is the last sample of the previous piece.
  while (this.pos < input.length - 1) {
    var i = Math.floor(this.pos);
    var t = this.pos - i;
    var a = i < 0 ? this.last : input[i];
    var b = input[i + 1];
    out.push(a + (b - a) * t);
    this.pos += this.ratio;
  }
  this.pos -= input.length;
  if (input.length) this.last = input[input.length - 1];
  return new Float32Array(out);
};

// Speech cleaning, after the v1 Android companion, which was tuned on real watches: the watch
// microphone is quiet (around -50 dBFS for normal speech) and has a DC offset, and the button
// presses that start and stop a recording click far louder than the words.
var CLEAN = {
  windowMs: 20,
  fadeMs: 30,
  target: 0.183,            // speech RMS after gain (6000 of 32768)
  maxWindowRms: 0.275,
  maxWindowPeak: 0.488,
  maxGain: 128,
  pauseRatio: 10,
  pauseFloor: 0.35,
  knee: 0.732,
  ceiling: 0.977,
};

function softLimit(x) {
  var m = Math.abs(x);
  if (m <= CLEAN.knee) return x;
  var room = CLEAN.ceiling - CLEAN.knee;
  return (x < 0 ? -1 : 1) * (CLEAN.knee + room * Math.tanh((m - CLEAN.knee) / room));
}

function percentile(sorted, p) { return sorted.length ? sorted[Math.floor((sorted.length - 1) * p)] : 0; }

/**
 * Makes quiet watch audio fit for a speech model, and for listening.
 * Mutes the first `leadMs` and last `tailMs` (button presses) with short fades, removes DC and
 * rumble below about 75 Hz, and sets one gain from the speech level (the 90th percentile of 20 ms
 * window RMS, but at least a third of the 98th) towards a fixed target, at most 128x. Windows far
 * louder than speech (clicks) are turned down, pauses gently lowered, rare peaks soft-limited.
 * Returns { samples, speech, floor, gain } with levels before gain, as 0..1 RMS.
 */
function cleanSpeech(samples, rate, options) {
  var o = options || {};
  var n = samples.length;
  var lead = Math.floor(rate * (o.leadMs || 0) / 1000);
  var tail = Math.floor(rate * (o.tailMs || 0) / 1000);
  var fade = Math.floor(rate * CLEAN.fadeMs / 1000);
  if (n < lead + tail + 8 * fade) { lead = 0; tail = 0; fade = 0; }
  var end = n - tail;
  var out = new Float32Array(n);
  if (end <= lead) return { samples: out, speech: 0, floor: 0, gain: 1 };

  var sum = 0;
  for (var i = lead; i < end; i++) sum += samples[i];
  var dc = sum / (end - lead);
  var pole = Math.exp(-2 * Math.PI * 75 / rate);
  var prevIn = 0, prevOut = 0;
  for (i = lead; i < end; i++) {
    var x = samples[i] - dc;
    var y = x - prevIn + pole * prevOut;
    prevIn = x;
    prevOut = y;
    var f = 1;
    if (fade > 0 && i < lead + fade) f = (i - lead + 1) / fade;
    else if (fade > 0 && i >= end - fade) f = (end - i) / fade;
    out[i] = y * f;
  }

  var win = Math.max(1, Math.floor(rate * CLEAN.windowMs / 1000));
  var windows = Math.max(1, Math.ceil((end - lead) / win));
  var rms = new Float32Array(windows);
  var peaks = new Float32Array(windows);
  for (var w = 0; w < windows; w++) {
    var from = lead + w * win, to = Math.min(end, from + win), e = 0, pk = 0;
    for (i = from; i < to; i++) {
      e += out[i] * out[i];
      if (Math.abs(out[i]) > pk) pk = Math.abs(out[i]);
    }
    rms[w] = to > from ? Math.sqrt(e / (to - from)) : 0;
    peaks[w] = pk;
  }
  var sorted = Array.prototype.slice.call(rms).sort(function (a, b) { return a - b; });
  var speech = Math.max(percentile(sorted, 0.9), percentile(sorted, 0.98) / 3);
  var floor = percentile(sorted, 0.1);
  var gain = speech < 1 / 32768 ? 1 : Math.min(CLEAN.target / speech, CLEAN.maxGain);

  var own = new Float32Array(windows);
  var quiet = speech / CLEAN.pauseRatio;
  for (w = 0; w < windows; w++) {
    var g = gain;
    if (rms[w] * g > CLEAN.maxWindowRms) g = CLEAN.maxWindowRms / rms[w];
    if (peaks[w] * g > CLEAN.maxWindowPeak) g = CLEAN.maxWindowPeak / peaks[w];
    if (quiet > 0 && rms[w] < quiet) g *= Math.max(CLEAN.pauseFloor, Math.sqrt(rms[w] / quiet));
    own[w] = g;
  }
  var wg = new Float32Array(windows);
  for (w = 0; w < windows; w++) {
    wg[w] = Math.min(own[w], own[Math.max(0, w - 1)], own[Math.min(windows - 1, w + 1)]);
  }
  for (i = lead; i < end; i++) {
    // Between window centres, so the gain never steps.
    var position = (i - lead) / win - 0.5;
    var w0 = Math.min(windows - 1, Math.max(0, Math.floor(position)));
    var w1 = Math.min(windows - 1, w0 + 1);
    var t = Math.min(1, Math.max(0, position - w0));
    out[i] = softLimit(out[i] * (wg[w0] + (wg[w1] - wg[w0]) * t));
  }
  return { samples: out, speech: speech, floor: floor, gain: gain };
}

var MEMO_LEAD_MS = 300;       // the press that started it
var MEMO_TAIL_MS = 200;       // the press that stopped it

/** A memo as the speech model should hear it: cleaned, at 16 kHz. Returns cleanSpeech's result. */
function memoForSpeech(bytes, options) {
  var cleaned = cleanSpeech(memoDecode8k(bytes), 8000, options || { leadMs: MEMO_LEAD_MS, tailMs: MEMO_TAIL_MS });
  cleaned.samples = upsample2x(cleaned.samples);
  return cleaned;
}

/** A memo as a 16-bit mono 8 kHz WAV file, cleaned for listening. */
function memoWav(bytes) {
  var pcm = cleanSpeech(memoDecode8k(bytes), 8000, { leadMs: MEMO_LEAD_MS, tailMs: MEMO_TAIL_MS }).samples;
  var out = new Uint8Array(44 + pcm.length * 2);
  var view = new DataView(out.buffer);
  var text = function (at, s) { for (var k = 0; k < s.length; k++) out[at + k] = s.charCodeAt(k); };
  text(0, 'RIFF'); view.setUint32(4, 36 + pcm.length * 2, true); text(8, 'WAVE');
  text(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, 8000, true); view.setUint32(28, 16000, true); view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  text(36, 'data'); view.setUint32(40, pcm.length * 2, true);
  for (var i = 0; i < pcm.length; i++) {
    var v = Math.round(pcm[i] * 32767);
    view.setInt16(44 + 2 * i, v > 32767 ? 32767 : v < -32768 ? -32768 : v, true);
  }
  return out;
}

/** Level in dBFS of a 0..1 RMS, rounded; -96 for silence. */
function dbfs(level) { return level > 1 / 32768 ? Math.round(20 * Math.log(level) / Math.LN10) : -96; }

/**
 * What a model said, or '' when it said nothing real: tags such as [BLANK_AUDIO] or (silence)
 * are dropped, and so is text with no letters or digits.
 */
function cleanTranscript(text) {
  var t = String(text || '').replace(/\[[^\]]*\]|\([^)]*\)/g, ' ').replace(/\s+/g, ' ').trim();
  // Letters of the scripts the models write; symbols such as music notes or ellipses are not words.
  return /[A-Za-z0-9\u00C0-\u024F\u0370-\u052F\u0590-\u06FF\u0900-\u0DFF\u0E00-\u0EFF\u3040-\u30FF\u3400-\u9FFF\uAC00-\uD7AF]/.test(t) ? t : '';
}

/**
 * Tells speech from silence in 16 kHz audio, 20 ms at a time.
 * `ended` turns true once speech has been followed by a long enough pause.
 *
 * The bar is set from the room: the first 400 ms (the user has only just pressed the button)
 * give the noise floor, which then follows the quiet frames. Speech must be three times the
 * floor, with a low absolute minimum because the watch microphone is quiet. DC is removed first.
 */
function Vad(options) {
  var o = options || {};
  this.pauseMs = o.pauseMs || 1600;
  this.patienceMs = o.patienceMs || 12000;   // never heard: give up, but not on quiet speech too soon
  this.minLevel = 0.0015;
  this.minFloor = 0.0003;
  this.ratio = 2.5;
  this.floor = 0;
  this.sinceLowMs = 0;
  this.calibration = [];
  this.speechMs = 0;
  this.silenceMs = 0;
  this.totalMs = 0;
  this.heard = false;
  this.rest = null;
  this.prevIn = 0;
  this.prevOut = 0;
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
    for (var j = i; j < i + frame; j++) {
      // A gentle high-pass: the microphone's offset is not sound.
      var y = data[j] - this.prevIn + 0.995 * this.prevOut;
      this.prevIn = data[j];
      this.prevOut = y;
      sum += y * y;
    }
    var rms = Math.sqrt(sum / frame);
    this.totalMs += 20;
    if (this.calibration) {
      this.calibration.push(rms);
      if (this.calibration.length < 20) continue;
      // The quiet end of the first 400 ms, so a word said straight away does not set the bar.
      var sorted = this.calibration.sort(function (a, b) { return a - b; });
      // Never zero: a stream that opens with digital silence must not make every hiss speech.
      this.floor = Math.max(this.minFloor, sorted[Math.floor(sorted.length * 0.2)]);
      this.calibration = null;
      continue;
    }
    var loud = rms > Math.max(this.floor * this.ratio, this.minLevel);
    if (loud) {
      this.speechMs += 20;
      this.silenceMs = 0;
      if (this.speechMs >= 160) this.heard = true;
    } else {
      this.silenceMs += 20;
      if (this.silenceMs > 200 && !this.heard) this.speechMs = 0;
      // The floor follows the quiet frames: down quickly; up slowly, and only from frames near it,
      // so soft speech between words is not taken for the room.
      if (rms < this.floor) this.floor = this.floor * 0.8 + rms * 0.2;
      else if (rms < this.floor * 1.5) this.floor = this.floor * 0.97 + rms * 0.03;
      if (this.floor < this.minFloor) this.floor = this.minFloor;
    }
    // Nobody talks for 4 s without a moment near the floor: past that, the room got louder, and the
    // floor leaks towards it so the end of speech can still be found.
    if (rms < this.floor * 1.2) this.sinceLowMs = 0;
    else this.sinceLowMs += 20;
    if (this.sinceLowMs > 4000) this.floor = this.floor * 0.98 + rms * 0.02;
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
  memoDecode8k: memoDecode8k, upsample2x: upsample2x, memoToFloat16k: memoToFloat16k,
  Resampler: Resampler, cleanSpeech: cleanSpeech, memoForSpeech: memoForSpeech, memoWav: memoWav,
  dbfs: dbfs, cleanTranscript: cleanTranscript,
  Vad: Vad, splitForModel: splitForModel, megabytes: megabytes,
};
