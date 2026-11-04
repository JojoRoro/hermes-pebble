#ifndef HERMES_MEMO_CORE_H
#define HERMES_MEMO_CORE_H

/* Each feature uses a different part of this file. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HVM1 voice memo: 16-byte header, then 8 kHz mono IMA ADPCM (low nibble first).
 * Header: "HVM1", u16 rate, u8 codec, u8 coverage (percent of the recording time for which
 * microphone audio arrived, 0 = not measured), u32 sample count, u32 watch time.
 * The microphone delivers 16 kHz signed 16-bit PCM at a low level (dictation boosts it
 * 3x), so the watch applies MEMO_INPUT_GAIN before coding: quiet speech then stays well
 * above ADPCM's smallest step, and only button clicks saturate.
 * A half-band filter drops it to 8 kHz before 4-bit coding, so one second costs 4,000
 * bytes instead of 32,000.
 *
 * Linear mode keeps the whole file (header first) in one buffer. Ring mode, used when the
 * memo leaves the watch while recording, keeps only ADPCM data in a circular buffer: length
 * counts every data byte produced, consumed every byte the caller has passed on, and crc is
 * the running CRC-32 of the data. The header then travels separately at the end. */
#define MEMO_HEADER 16u
#define MEMO_RATE 8000u
#define MEMO_CODEC_IMA_ADPCM 1u
#define MEMO_BYTES_PER_SECOND (MEMO_RATE / 2u)
#define MEMO_FILTER_TAPS 7u
#define MEMO_INPUT_GAIN 4

typedef struct {
  uint8_t *bytes;
  uint32_t capacity, length, samples;
  uint32_t consumed, crc; /* Ring mode: bytes passed on, running CRC-32 (pre-inversion). */
  int32_t predictor;
  int16_t history[MEMO_FILTER_TAPS];
  uint8_t filled, index;
  uint8_t coverage; /* Set by the caller before memo_encoder_finish; 0 = not measured. */
  bool odd, high_nibble;
  bool full, ring;
} MemoEncoder;

static const int16_t s_memo_steps[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
  11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t s_memo_index_steps[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void memo_put_u32(uint8_t *bytes, uint32_t offset, uint32_t value) {
  for (uint8_t i = 0; i < 4u; i++) bytes[offset + i] = (uint8_t)(value >> (8u * i));
}

static uint32_t memo_crc_byte(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (uint8_t b = 0; b < 8u; b++) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  return crc;
}

static uint32_t memo_crc32(const uint8_t *bytes, uint32_t length) {
  uint32_t crc = 0xffffffffu;
  for (uint32_t i = 0; i < length; i++) crc = memo_crc_byte(crc, bytes[i]);
  return ~crc;
}

/* Starts a memo in caller-owned memory. Capacity includes the header. */
static bool memo_encoder_begin(MemoEncoder *encoder, uint8_t *bytes, uint32_t capacity) {
  if (encoder == NULL || bytes == NULL || capacity <= MEMO_HEADER) return false;
  *encoder = (MemoEncoder){ .bytes = bytes, .capacity = capacity, .length = MEMO_HEADER };
  return true;
}

/* Starts a ring-mode memo: capacity bytes of ADPCM data, no header in the buffer. */
static bool memo_encoder_begin_ring(MemoEncoder *encoder, uint8_t *bytes, uint32_t capacity) {
  if (encoder == NULL || bytes == NULL || capacity == 0u) return false;
  *encoder = (MemoEncoder){ .bytes = bytes, .capacity = capacity, .crc = 0xffffffffu, .ring = true };
  return true;
}

/* Ring mode: data bytes produced but not yet passed on. */
static uint32_t memo_ring_pending(const MemoEncoder *encoder) { return encoder->length - encoder->consumed; }

/* Ring mode: copies count bytes starting at data offset `from` (which must still be held). */
static void memo_ring_copy(const MemoEncoder *encoder, uint32_t from, uint8_t *out, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) out[i] = encoder->bytes[(from + i) % encoder->capacity];
}

static uint8_t memo_adpcm_nibble(MemoEncoder *encoder, int32_t sample) {
  int32_t step = s_memo_steps[encoder->index];
  int32_t diff = sample - encoder->predictor;
  int32_t delta = step >> 3;
  uint8_t code = 0u;
  if (diff < 0) { code = 8u; diff = -diff; }
  if (diff >= step) { code |= 4u; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { code |= 2u; diff -= step; delta += step; }
  step >>= 1;
  if (diff >= step) { code |= 1u; delta += step; }
  encoder->predictor += (code & 8u) ? -delta : delta;
  if (encoder->predictor > 32767) encoder->predictor = 32767;
  if (encoder->predictor < -32768) encoder->predictor = -32768;
  int32_t index = (int32_t)encoder->index + s_memo_index_steps[code & 7u];
  encoder->index = (uint8_t)(index < 0 ? 0 : index > 88 ? 88 : index);
  return code;
}

static void memo_emit(MemoEncoder *encoder, int32_t sample) {
  if (encoder->full) return;
  uint32_t used = encoder->ring ? memo_ring_pending(encoder) : encoder->length;
  if (!encoder->high_nibble && used >= encoder->capacity) {
    encoder->full = true;
    return;
  }
  uint32_t at = encoder->ring ? encoder->length % encoder->capacity : encoder->length;
  uint8_t code = memo_adpcm_nibble(encoder, sample);
  if (encoder->high_nibble) {
    encoder->bytes[at] |= (uint8_t)(code << 4);
    encoder->crc = memo_crc_byte(encoder->crc, encoder->bytes[at]);
    encoder->length++;
  } else {
    encoder->bytes[at] = code;
  }
  encoder->high_nibble = !encoder->high_nibble;
  encoder->samples++;
  if (!encoder->ring && !encoder->high_nibble && encoder->length >= encoder->capacity) encoder->full = true;
}

/* Feeds 16 kHz PCM. Returns false once the buffer is full; later samples are ignored.
 * Half-band low-pass (-1, 0, 9, 16, 9, 0, -1) / 32 times MEMO_INPUT_GAIN, saturating,
 * keeping every second output. */
static bool memo_encoder_push(MemoEncoder *encoder, const int16_t *samples, size_t count) {
  if (encoder == NULL || encoder->bytes == NULL) return false;
  for (size_t i = 0; i < count && !encoder->full; i++) {
    for (uint8_t t = MEMO_FILTER_TAPS - 1u; t > 0u; t--) encoder->history[t] = encoder->history[t - 1u];
    encoder->history[0] = samples[i];
    if (encoder->filled < MEMO_FILTER_TAPS) encoder->filled++;
    encoder->odd = !encoder->odd;
    if (encoder->filled < MEMO_FILTER_TAPS || !encoder->odd) continue;
    const int16_t *h = encoder->history;
    int32_t y = 16 * (int32_t)h[3] + 9 * ((int32_t)h[2] + h[4]) - ((int32_t)h[0] + h[6]);
    y = y * MEMO_INPUT_GAIN / 32;
    memo_emit(encoder, y > 32767 ? 32767 : y < -32768 ? -32768 : y);
  }
  return !encoder->full;
}

static void memo_write_header(const MemoEncoder *encoder, uint8_t *b, uint32_t recorded_at) {
  b[0] = 'H'; b[1] = 'V'; b[2] = 'M'; b[3] = '1';
  b[4] = (uint8_t)(MEMO_RATE & 0xffu); b[5] = (uint8_t)(MEMO_RATE >> 8);
  b[6] = MEMO_CODEC_IMA_ADPCM; b[7] = encoder->coverage;
  memo_put_u32(b, 8u, encoder->samples);
  memo_put_u32(b, 12u, recorded_at);
}

/* Completes the last byte. Linear mode also writes the header and returns the file length
 * (header plus data); ring mode returns the data length and leaves the header to the caller. */
static uint32_t memo_encoder_finish(MemoEncoder *encoder, uint32_t recorded_at) {
  if (encoder == NULL || encoder->bytes == NULL) return 0u;
  if (encoder->high_nibble) {
    /* Odd sample count: the high nibble of the last byte stays zero. */
    uint32_t at = encoder->ring ? encoder->length % encoder->capacity : encoder->length;
    encoder->crc = memo_crc_byte(encoder->crc, encoder->bytes[at]);
    encoder->length++;
    encoder->high_nibble = false;
  }
  if (!encoder->ring) memo_write_header(encoder, encoder->bytes, recorded_at);
  return encoder->length;
}

/* Watch-speaker playback: ADPCM in, signed 8-bit PCM out, one 20 ms window at a time. Mirrors the
 * phone's live chain with integers: the first 300 ms are muted (button press), DC and rumble are
 * removed, the gain follows the speech level, a window that would clip is turned down on its own
 * so clicks do not duck what follows, and pauses are lowered. */
#define MEMO_PLAY_WINDOW 160u
#define MEMO_PLAY_LEAD_MUTE 2400u
#define MEMO_PLAY_TARGET 5000
#define MEMO_PLAY_LEVEL_FLOOR 40
#define MEMO_PLAY_MAX_GAIN_Q8 (64 * 256)
#define MEMO_PLAY_PEAK 30000

typedef struct {
  int32_t predictor, previous_in, previous_out, level;
  uint32_t produced;
  uint16_t filled;
  uint8_t index;
  int16_t window[MEMO_PLAY_WINDOW];
} MemoPlayback;

/* Turns the current window into PCM at out[(*head)++ % capacity]. */
static void memo_playback_flush(MemoPlayback *p, int8_t *out, uint32_t capacity, uint32_t *head) {
  if (p->filled == 0u) return;
  int32_t sum = 0, peak = 0;
  for (uint16_t i = 0; i < p->filled; i++) {
    int32_t v = p->window[i] < 0 ? -(int32_t)p->window[i] : p->window[i];
    sum += v;
    if (v > peak) peak = v;
  }
  int32_t mean = sum / p->filled;
  if (mean > 0) p->level += mean > p->level ? (mean - p->level) / 4 : -(p->level / 64);
  int32_t level = p->level < MEMO_PLAY_LEVEL_FLOOR ? MEMO_PLAY_LEVEL_FLOOR : p->level;
  int32_t gain = MEMO_PLAY_TARGET * 256 / level;
  if (gain < 256) gain = 256;
  if (gain > MEMO_PLAY_MAX_GAIN_Q8) gain = MEMO_PLAY_MAX_GAIN_Q8;
  if (peak > 0 && (int64_t)peak * gain > (int64_t)MEMO_PLAY_PEAK * 256) gain = MEMO_PLAY_PEAK * 256 / peak;
  if (mean * 10 < p->level) gain /= 3;
  for (uint16_t i = 0; i < p->filled; i++) out[(*head)++ % capacity] = (int8_t)((p->window[i] * gain) >> 16);
  p->filled = 0u;
}

static void memo_playback_nibble(MemoPlayback *p, uint8_t code, int8_t *out, uint32_t capacity, uint32_t *head) {
  int32_t step = s_memo_steps[p->index];
  int32_t delta = step >> 3;
  if (code & 4u) delta += step;
  if (code & 2u) delta += step >> 1;
  if (code & 1u) delta += step >> 2;
  p->predictor += (code & 8u) ? -delta : delta;
  if (p->predictor > 32767) p->predictor = 32767;
  if (p->predictor < -32768) p->predictor = -32768;
  int32_t index = (int32_t)p->index + s_memo_index_steps[code & 7u];
  p->index = (uint8_t)(index < 0 ? 0 : index > 88 ? 88 : index);
  int32_t y = p->predictor - p->previous_in + p->previous_out * 15 / 16;
  p->previous_in = p->predictor;
  p->previous_out = y;
  if (y > 32767) y = 32767;
  if (y < -32768) y = -32768;
  p->window[p->filled++] = p->produced++ < MEMO_PLAY_LEAD_MUTE ? 0 : (int16_t)y;
  if (p->filled == MEMO_PLAY_WINDOW) memo_playback_flush(p, out, capacity, head);
}

/* Decodes count ADPCM bytes. The caller keeps at least (count * 2 + 160) free in out. */
static void memo_playback_feed(MemoPlayback *p, const uint8_t *bytes, uint32_t count, int8_t *out, uint32_t capacity, uint32_t *head) {
  for (uint32_t i = 0; i < count; i++) {
    memo_playback_nibble(p, bytes[i] & 0x0fu, out, capacity, head);
    memo_playback_nibble(p, bytes[i] >> 4, out, capacity, head);
  }
}

static uint32_t memo_encoder_seconds(const MemoEncoder *encoder) {
  return encoder == NULL ? 0u : encoder->samples / MEMO_RATE;
}
#endif
