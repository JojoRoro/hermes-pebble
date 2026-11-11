// Pure helpers in src/pkjs/lib.js.
import test from 'node:test';
import assert from 'node:assert/strict';
import { lib } from './env.mjs';

const SAMPLES = ['', 'plain ascii', 'Zürich – “quotes”', 'ł€中文', 'emoji 🎉 and 𝄞 clef', 'mixed é🎉z\n\nnew'];

test('utf8 round trip matches Node, multibyte and astral included', () => {
  for (const text of SAMPLES) {
    const bytes = lib.utf8Encode(text);
    assert.deepEqual(Buffer.from(bytes), Buffer.from(text, 'utf8'), text);
    assert.equal(lib.utf8Decode(bytes), text);
  }
  const bytes = lib.utf8Encode('aé🎉b');
  assert.equal(lib.utf8Decode(bytes, 1, 3), 'é');
  assert.equal(lib.utf8Decode(bytes, 3, 7), '🎉');
});

test('clipBytes never splits a character and never exceeds the limit', () => {
  const text = 'aé€🎉'.repeat(20);
  for (let max = 0; max < 60; max++) {
    const clipped = lib.clipBytes(text, max);
    const n = Buffer.byteLength(clipped);
    assert.ok(n <= max, `max ${max}: ${n}`);
    assert.ok(text.startsWith(clipped));
    assert.ok(!clipped.includes('�'));
    // As long as possible: the next character would not have fitted.
    const nextChar = String.fromCodePoint(text.slice(clipped.length).codePointAt(0));
    assert.ok(n + Buffer.byteLength(nextChar) > max, `max ${max} cut too early`);
  }
  assert.equal(lib.clipBytes('short', 100), 'short');
  assert.equal(lib.clipBytes(null, 5), '');
});

test('crc32 known vectors', () => {
  assert.equal(lib.crc32(lib.utf8Encode('123456789')), 0xCBF43926);
  assert.equal(lib.crc32([]), 0);
  assert.equal(lib.crc32(lib.utf8Encode('The quick brown fox jumps over the lazy dog')), 0x414FA339);
  assert.equal(lib.crc32(new Uint8Array([0xff, 0, 0x80])), lib.crc32([0xff, 0, 0x80]));
});

test('base64 matches Node for every length and decodes back', () => {
  for (let n = 0; n < 40; n++) {
    const bytes = Array.from({ length: n }, (_, i) => (i * 73 + 41) & 255);
    const encoded = lib.base64Encode(bytes);
    assert.equal(encoded, Buffer.from(bytes).toString('base64'), `length ${n}`);
    assert.deepEqual(lib.base64Decode(encoded), bytes);
    assert.deepEqual(lib.base64Decode(encoded.replace(/=/g, '')), bytes, 'padding is optional');
  }
  assert.deepEqual(lib.base64Decode('AA E=\n'), [0, 1], 'whitespace is ignored');
  assert.equal(lib.base64Encode(new Uint8Array([1, 2, 3, 4])), 'AQIDBA==');
});

function pagesOf(text, limit) {
  const bytes = lib.utf8Encode(text);
  const pages = [];
  let offset = 0;
  for (let guard = 0; guard < 10000; guard++) {
    const page = lib.pageAt(bytes, offset, limit);
    if (!page.text && page.next >= bytes.length) break;
    pages.push(Object.assign({ offset }, page));
    assert.ok(page.next > offset, 'always moves forward');
    offset = page.next;
    if (offset >= bytes.length) break;
  }
  return { bytes, pages };
}

const strip = (s) => s.replace(/\s+/g, '');

test('pageAt covers the whole text and never splits a UTF-8 sequence', () => {
  const words = ['Zürich', 'ł', '€uro', '中文字', 'plain', 'words.', 'end!', 'q?', '🎉'];
  let text = '';
  for (let i = 0; i < 900; i++) {
    text += words[(i * 7) % words.length];
    text += i % 37 === 36 ? '\n\n' : i % 11 === 10 ? '\n' : ' ';
  }
  for (const limit of [16, 40, 97, 200, lib.PAGE_BYTES]) {
    const { bytes, pages } = pagesOf(text, limit);
    for (const page of pages) {
      const n = Buffer.byteLength(page.text);
      assert.ok(n <= limit, `limit ${limit}: page of ${n}`);
      assert.ok(!page.text.includes('�'));
      assert.equal(page.text, page.text.trim(), 'breaks are dropped');
      // The page is exactly the bytes it claims, at a character boundary.
      assert.equal((bytes[page.next] & 0xC0) === 0x80, false);
    }
    assert.equal(strip(pages.map((p) => p.text).join('')), strip(text), `limit ${limit} loses nothing`);
  }
});

test('pageAt prefers paragraph, then sentence, then word breaks', () => {
  const para = 'First paragraph is here. ' + 'x'.repeat(10) + '\n\nSecond paragraph goes on and on.';
  let page = lib.pageAt(lib.utf8Encode(para), 0, 60);
  assert.equal(page.text, 'First paragraph is here. xxxxxxxxxx');
  let next = lib.pageAt(lib.utf8Encode(para), page.next, 60);
  assert.equal(next.paragraph, true);
  assert.equal(next.text, 'Second paragraph goes on and on.');

  // Breaks are looked for in the second half of a page only, so pages are never tiny.
  const sentences = 'One two three four. Five six seven eight nine ten eleven';
  page = lib.pageAt(lib.utf8Encode(sentences), 0, 30);
  assert.equal(page.text, 'One two three four.');
  assert.equal(lib.pageAt(lib.utf8Encode('One two. Three four five six seven eight'), 0, 30).text,
    'One two. Three four five six', 'too early a sentence end gives way to a word break');
  next = lib.pageAt(lib.utf8Encode(sentences), page.next, 30);
  assert.equal(next.paragraph, false);

  const words = 'alpha beta gamma delta epsilon zeta eta theta';
  page = lib.pageAt(lib.utf8Encode(words), 0, 20);
  assert.equal(page.text, 'alpha beta gamma');

  const solid = '€'.repeat(50);
  page = lib.pageAt(lib.utf8Encode(solid), 0, 20);
  assert.equal(page.text, '€'.repeat(6), 'no break at all: cut at a character boundary');

  const blank = lib.pageAt(lib.utf8Encode('  '), 0, 20);
  assert.equal(blank.text, '');
  assert.equal(blank.next, 2);
  assert.equal(lib.pageAt(lib.utf8Encode('short'), 0).text, 'short');
});

// ---- audio -------------------------------------------------------------------------------------

const STEPS = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
  544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
  2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
  12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

/** A textbook IMA ADPCM encoder, written here independently. Returns bytes and what it decoded to. */
function encodeAdpcm(pcm) {
  let predicted = 0, index = 0;
  const bytes = new Uint8Array(Math.ceil(pcm.length / 2));
  const decoded = [];
  for (let i = 0; i < pcm.length; i++) {
    const step = STEPS[index];
    let diff = pcm[i] - predicted;
    let code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    predicted += code & 8 ? -delta : delta;
    predicted = Math.max(-32768, Math.min(32767, predicted));
    index = Math.max(0, Math.min(88, index + INDEX[code]));
    decoded.push(predicted);
    bytes[i >> 1] |= i & 1 ? code << 4 : code;     // low nibble first
  }
  return { bytes, decoded };
}

test('memo ADPCM decodes to what a reference encoder reconstructs, at 16 kHz', () => {
  const pcm = [];
  for (let i = 0; i < 8000; i++) {
    pcm.push(Math.round(12000 * Math.sin(i * 2 * Math.PI * 440 / 8000) + 3000 * Math.sin(i * 0.013)));
  }
  pcm.push(32767, 32767, -32768, -32768, 0, 0);       // clipping either way
  const { bytes, decoded } = encodeAdpcm(pcm);
  const out = lib.memoToFloat16k(bytes);
  assert.equal(out.length, bytes.length * 4);
  let previous = 0;
  for (let i = 0; i < decoded.length; i++) {
    const sample = decoded[i] / 32768;
    assert.ok(Math.abs(out[2 * i + 1] - sample) < 1e-6, `sample ${i}`);
    assert.ok(Math.abs(out[2 * i] - (previous + sample) / 2) < 1e-6, `midpoint ${i}`);
    previous = sample;
  }
  // And the reconstruction follows the signal it encoded.
  let err = 0;
  for (let i = 100; i < 8000; i++) err += Math.abs(decoded[i] - pcm[i]);
  assert.ok(err / 7900 < 600, 'mean error ' + err / 7900);
});

function tone(seconds, amplitude) {
  const out = new Float32Array(Math.round(seconds * 16000));
  for (let i = 0; i < out.length; i++) out[i] = amplitude * Math.sin(i * 2 * Math.PI * 220 / 16000);
  return out;
}

function noise(seconds, amplitude, seed = 1) {
  const out = new Float32Array(Math.round(seconds * 16000));
  let s = seed;
  for (let i = 0; i < out.length; i++) { s = (s * 1103515245 + 12345) & 0x7fffffff; out[i] = amplitude * (s / 0x7fffffff * 2 - 1); }
  return out;
}

/** Feeds audio in uneven chunks, as the stream delivers it. */
function feed(vad, samples) {
  for (let i = 0; i < samples.length; i += 777) vad.feed(samples.subarray(i, Math.min(samples.length, i + 777)));
}

test('VAD: speech then a long enough pause ends it, a short pause does not', () => {
  const vad = new lib.Vad({ pauseMs: 1600 });
  feed(vad, noise(0.5, 0.002));
  assert.equal(vad.heard, false);
  feed(vad, tone(1.0, 0.2));
  assert.equal(vad.heard, true);
  feed(vad, noise(1.0, 0.002));
  assert.equal(vad.ended(), false, 'a breath is not the end');
  feed(vad, tone(0.5, 0.2));
  feed(vad, noise(1.4, 0.002));
  assert.equal(vad.ended(), false);
  feed(vad, noise(0.3, 0.002));
  assert.equal(vad.ended(), true);
});

test('VAD: clicks are not speech, and silence gives up after the patience time', () => {
  const vad = new lib.Vad({ pauseMs: 1600, patienceMs: 8000 });
  for (let i = 0; i < 10; i++) {
    feed(vad, tone(0.06, 0.3));           // 60 ms clicks
    feed(vad, noise(0.5, 0.002));
  }
  assert.equal(vad.heard, false);
  assert.equal(vad.ended(), false);
  feed(vad, noise(2.5, 0.002));
  assert.equal(vad.ended(), true, 'about 8 s with nothing said');
});

test('VAD: a steady noisy room raises the bar', () => {
  const vad = new lib.Vad({});
  feed(vad, noise(3, 0.01));
  assert.equal(vad.heard, false, 'steady background noise is not speech');
  feed(vad, tone(0.5, 0.25));
  assert.equal(vad.heard, true);
});

test('splitForModel: pieces fit the model, cover everything, and cut where it is quiet', () => {
  const short = new Float32Array(16000 * 10);
  assert.equal(lib.splitForModel(short, 25).length, 1);

  // 60 s of tone with a quiet gap at 22 s and 46 s.
  const samples = tone(60, 0.3);
  for (const at of [22, 46]) samples.fill(0, at * 16000, at * 16000 + 4000);
  const parts = lib.splitForModel(samples, 25);
  assert.equal(parts.length, 3);
  let total = 0;
  for (const part of parts) {
    assert.ok(part.length <= 25 * 16000);
    total += part.length;
  }
  assert.equal(total, samples.length);
  const firstCut = parts[0].length / 16000;
  assert.ok(firstCut > 22 && firstCut < 22.25, 'cut inside the gap: ' + firstCut);
  // Pieces are views: in order and contiguous.
  assert.equal(parts[1].byteOffset, parts[0].byteOffset + parts[0].byteLength);
});

test('megabytes and isFinal', () => {
  assert.equal(lib.megabytes(32079632), '31 MB');
  assert.equal(lib.isFinal(lib.STATE.ANSWERED), true);
  assert.equal(lib.isFinal(lib.STATE.APPROVAL), false);
  assert.equal(lib.isFinal(lib.STATE.UNKNOWN), false);
});

// ---- speech cleaning (after the v1 companion, tuned on real watches) ----------------------------

function rms(samples, from = 0, to = samples.length) {
  let e = 0;
  for (let i = from; i < to; i++) e += samples[i] * samples[i];
  return Math.sqrt(e / Math.max(1, to - from));
}

test('cleanSpeech: quiet speech is brought up, DC and the button clicks go', () => {
  const rate = 16000;
  // -50 dBFS "speech" on a DC offset, with loud clicks where the buttons were pressed.
  const n = rate * 3;
  const x = new Float32Array(n);
  for (let i = 0; i < n; i++) x[i] = 0.05 + 0.0045 * Math.sin(i * 2 * Math.PI * 300 / rate);
  for (let i = 0; i < 800; i++) x[i] += i % 2 ? 0.8 : -0.8;            // start press
  for (let i = n - 800; i < n; i++) x[i] += i % 2 ? 0.8 : -0.8;        // stop press
  const out = lib.cleanSpeech(x, rate, { leadMs: 300, tailMs: 200 });
  assert.equal(out.samples.length, n);
  assert.ok(out.gain > 20 && out.gain <= 128, 'gain ' + out.gain);
  assert.ok(lib.dbfs(out.speech) < -45, 'speech measured before gain: ' + lib.dbfs(out.speech));
  const middle = rms(out.samples, rate, 2 * rate);
  assert.ok(middle > 0.1 && middle < 0.3, 'speech level after: ' + middle);
  let mean = 0;
  for (let i = rate; i < 2 * rate; i++) mean += out.samples[i];
  assert.ok(Math.abs(mean / rate) < 0.005, 'no DC left');
  assert.equal(rms(out.samples, 0, 4800), 0, 'start press muted');
  assert.equal(rms(out.samples, n - 3200, n), 0, 'stop press muted');
  for (const v of out.samples) assert.ok(Math.abs(v) < 0.98, 'never clips');
});

test('cleanSpeech: a click in the middle does not set the gain, and is held down', () => {
  const rate = 8000;
  const x = new Float32Array(rate * 2);
  for (let i = 0; i < x.length; i++) x[i] = 0.003 * Math.sin(i * 2 * Math.PI * 250 / rate);
  for (let i = 8000; i < 8040; i++) x[i] = 0.9;
  const out = lib.cleanSpeech(x, rate, {});
  assert.ok(out.gain > 30, 'gain from the speech, not the click: ' + out.gain);
  let peak = 0;
  for (let i = 7900; i < 8200; i++) peak = Math.max(peak, Math.abs(out.samples[i]));
  assert.ok(peak < 0.6, 'click held down: ' + peak);
});

test('cleanSpeech: silence and very short input are safe', () => {
  const zero = lib.cleanSpeech(new Float32Array(16000), 16000, { leadMs: 300, tailMs: 200 });
  assert.equal(zero.gain, 1);
  assert.equal(rms(zero.samples), 0);
  const tiny = lib.cleanSpeech(new Float32Array(10), 16000, { leadMs: 300, tailMs: 200 });
  assert.equal(tiny.samples.length, 10);
});

test('memoForSpeech and memoWav: a memo becomes 16 kHz model input and a playable WAV', () => {
  const pcm = [];
  for (let i = 0; i < 8000; i++) pcm.push(Math.round(600 * Math.sin(i * 2 * Math.PI * 300 / 8000)));
  const { bytes } = encodeAdpcm(pcm);
  const speech = lib.memoForSpeech(bytes);
  assert.equal(speech.samples.length, bytes.length * 4);
  assert.ok(speech.gain > 5, 'quiet memo is brought up');
  const wav = lib.memoWav(bytes);
  const view = new DataView(wav.buffer);
  assert.equal(String.fromCharCode(...wav.slice(0, 4)), 'RIFF');
  assert.equal(String.fromCharCode(...wav.slice(8, 12)), 'WAVE');
  assert.equal(view.getUint32(24, true), 8000, 'sample rate');
  assert.equal(view.getUint16(34, true), 16, 'bits');
  assert.equal(view.getUint32(40, true), bytes.length * 4, 'data bytes');
  assert.equal(wav.length, 44 + bytes.length * 4);
});

test('VAD: speech from a quiet microphone is heard', () => {
  // The watch delivers normal speech around -50 dBFS on a hiss near -66 dBFS.
  const vad = new lib.Vad({ pauseMs: 1600 });
  feed(vad, noise(0.5, 0.0009));
  feed(vad, tone(1.0, 0.0045));
  assert.equal(vad.heard, true);
  feed(vad, noise(1.7, 0.0009));
  assert.equal(vad.ended(), true);
});

test('VAD: a DC offset is neither speech nor in the way of it', () => {
  const vad = new lib.Vad({ pauseMs: 1600 });
  const offset = (samples) => samples.map((v) => v + 0.05);
  feed(vad, offset(noise(2, 0.0009)));
  assert.equal(vad.heard, false);
  feed(vad, offset(tone(0.5, 0.01)));
  assert.equal(vad.heard, true);
});

test('Resampler: 8 kHz in pieces to 16 kHz', () => {
  const r = new lib.Resampler(8000, 16000);
  const input = new Float32Array(800).map((_, i) => Math.sin(i * 2 * Math.PI * 200 / 8000));
  const out = [];
  for (let i = 0; i < input.length; i += 123) out.push(...r.feed(input.subarray(i, i + 123)));
  assert.ok(Math.abs(out.length - 1600) <= 2, 'length ' + out.length);
  for (let i = 10; i < out.length - 10; i++) {
    const expected = Math.sin((i / 2) * 2 * Math.PI * 200 / 8000);
    assert.ok(Math.abs(out[i] - expected) < 0.02, `sample ${i}: ${out[i]} vs ${expected}`);
  }
  assert.equal(new lib.Resampler(16000, 16000).feed(input), input);
});

test('cleanTranscript drops tags and empty text', () => {
  assert.equal(lib.cleanTranscript(' [BLANK_AUDIO] '), '');
  assert.equal(lib.cleanTranscript('(silence)'), '');
  assert.equal(lib.cleanTranscript('...'), '');
  assert.equal(lib.cleanTranscript('Buy milk [Music] tomorrow.'), 'Buy milk tomorrow.');
  assert.equal(lib.cleanTranscript('Grüße'), 'Grüße');
});

test('VAD: soft speech between words does not lift the floor until it is lost', () => {
  // Hiss at -60 dBFS, speech at -50 dBFS with soft gaps: it must last until the speaker stops.
  const vad = new lib.Vad({ pauseMs: 1600 });
  feed(vad, noise(1, 0.0017));
  for (let i = 0; i < 16; i++) {
    feed(vad, tone(0.15, 0.0045));
    feed(vad, noise(0.1, 0.0030));
  }
  assert.equal(vad.heard, true);
  assert.equal(vad.ended(), false, 'still talking after 4 s');
  feed(vad, noise(1.8, 0.0017));
  assert.equal(vad.ended(), true);
});

test('VAD: digital silence at the start does not make every hiss speech for good', () => {
  const vad = new lib.Vad({ pauseMs: 1600 });
  feed(vad, new Float32Array(16000));
  feed(vad, noise(8, 0.004));
  assert.equal(vad.ended(), true, 'a louder room is relearned and the listen can end');
});
