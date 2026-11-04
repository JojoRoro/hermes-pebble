// Host checks for the voice memo codec (no Pebble SDK needed).
// Optional argument: path for an HVM1 fixture that VoiceMemoCheck.kt decodes.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/c/memo_core.h"

#define PI 3.14159265358979323846

static uint32_t read_u32(const uint8_t *b, uint32_t o) {
  return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

/* Reference decoder, identical to VoiceMemoCodec.decode. */
static void decode(const uint8_t *memo, uint32_t samples, int16_t *out) {
  int32_t predictor = 0, index = 0;
  for (uint32_t i = 0; i < samples; i++) {
    uint8_t byte = memo[MEMO_HEADER + i / 2u];
    uint8_t code = (i & 1u) ? byte >> 4 : byte & 0x0fu;
    int32_t step = s_memo_steps[index], delta = step >> 3;
    if (code & 4u) delta += step;
    if (code & 2u) delta += step >> 1;
    if (code & 1u) delta += step >> 2;
    predictor += (code & 8u) ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    index += s_memo_index_steps[code & 7u];
    index = index < 0 ? 0 : index > 88 ? 88 : index;
    out[i] = (int16_t)predictor;
  }
}

static void feed_tone(MemoEncoder *encoder, double frequency, double amplitude, uint32_t count, uint32_t *phase) {
  int16_t frame[320];
  for (uint32_t done = 0; done < count; done += 320u) {
    uint32_t n = count - done < 320u ? count - done : 320u;
    for (uint32_t i = 0; i < n; i++, (*phase)++) frame[i] = (int16_t)lrint(amplitude * sin(2.0 * PI * frequency * *phase / 16000.0));
    memo_encoder_push(encoder, frame, n);
  }
}

static double tone_snr_db(const int16_t *pcm, uint32_t count, double frequency) {
  // Least-squares fit of a sine at the known frequency, after the codec settles.
  double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, yy = 0;
  for (uint32_t i = 400; i < count; i++) {
    double s = sin(2.0 * PI * frequency * i / 8000.0), c = cos(2.0 * PI * frequency * i / 8000.0), y = pcm[i];
    ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c; yy += y * y;
  }
  double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
  double signal = a * a * ss + 2 * a * b * sc + b * b * cc;
  double noise = yy - signal;
  return 10.0 * log10(signal / (noise > 1 ? noise : 1));
}

int main(int argc, char **argv) {
  assert(memo_crc32((const uint8_t *)"123456789", 9u) == 0xcbf43926u);

  static uint8_t buffer[MEMO_HEADER + 2u * MEMO_BYTES_PER_SECOND];
  MemoEncoder encoder;
  assert(!memo_encoder_begin(&encoder, buffer, MEMO_HEADER));
  assert(memo_encoder_begin(&encoder, buffer, sizeof(buffer)));

  // One second of a 440 Hz tone at 16 kHz becomes ~8,000 samples at 8 kHz.
  uint32_t phase = 0;
  // Inputs are a quarter of the decoded level: the encoder applies MEMO_INPUT_GAIN (4x).
  feed_tone(&encoder, 440.0, 3000.0, 16000u, &phase);
  assert(!encoder.full);
  assert(encoder.samples >= 7990u && encoder.samples <= 8000u);
  uint32_t length = memo_encoder_finish(&encoder, 1234567u);
  assert(length == MEMO_HEADER + (encoder.samples + 1u) / 2u);
  assert(memcmp(buffer, "HVM1", 4) == 0 && buffer[4] == 0x40 && buffer[5] == 0x1f && buffer[6] == MEMO_CODEC_IMA_ADPCM);
  assert(read_u32(buffer, 8) == encoder.samples && read_u32(buffer, 12) == 1234567u);
  assert(memo_encoder_seconds(&encoder) == 0u || memo_encoder_seconds(&encoder) == 1u);

  static int16_t pcm[16000];
  decode(buffer, encoder.samples, pcm);
  double snr = tone_snr_db(pcm, encoder.samples, 440.0);
  assert(snr > 20.0);

  // A 7 kHz tone lies above the 4 kHz output band; the half-band filter must suppress it.
  assert(memo_encoder_begin(&encoder, buffer, sizeof(buffer)));
  phase = 0;
  feed_tone(&encoder, 7000.0, 3000.0, 16000u, &phase);
  uint32_t alias_samples = encoder.samples;
  memo_encoder_finish(&encoder, 0u);
  decode(buffer, alias_samples, pcm);
  double energy = 0;
  for (uint32_t i = 400; i < alias_samples; i++) energy += (double)pcm[i] * pcm[i];
  double rms = sqrt(energy / (alias_samples - 400u));
  assert(rms < 12000.0 / sqrt(2.0) * 0.12); // At least ~18 dB down.

  // Capacity is a hard bound: overflow stops cleanly with an even or odd final sample.
  static uint8_t tiny[MEMO_HEADER + 101u];
  assert(memo_encoder_begin(&encoder, tiny, sizeof(tiny)));
  phase = 0;
  int16_t loud[320];
  for (int i = 0; i < 320; i++) loud[i] = (int16_t)(i % 2 ? 30000 : -30000);
  bool room = true;
  for (int i = 0; i < 20 && room; i++) room = memo_encoder_push(&encoder, loud, 320u);
  assert(!room && encoder.full && encoder.length == sizeof(tiny) && encoder.samples == 202u);
  assert(!memo_encoder_push(&encoder, loud, 320u) && encoder.samples == 202u);
  assert(memo_encoder_finish(&encoder, 0u) == sizeof(tiny));

  // A full-scale click saturates instead of wrapping around.
  assert(memo_encoder_begin(&encoder, buffer, sizeof(buffer)));
  for (int i = 0; i < 320; i++) loud[i] = 30000;
  for (int i = 0; i < 10; i++) memo_encoder_push(&encoder, loud, 320u);
  uint32_t click_samples = encoder.samples;
  memo_encoder_finish(&encoder, 0u);
  decode(buffer, click_samples, pcm);
  for (uint32_t i = 200; i < click_samples; i++) assert(pcm[i] > 30000);

  // Ring mode: a 1,000-byte ring drained in 300-byte pieces yields the same ADPCM as linear
  // mode, with a running CRC over the data and no header in the buffer.
  static uint8_t ring[1000], drained[MEMO_HEADER + 2u * MEMO_BYTES_PER_SECOND];
  MemoEncoder ringed;
  assert(!memo_encoder_begin_ring(&ringed, ring, 0u));
  assert(memo_encoder_begin_ring(&ringed, ring, sizeof(ring)) && ringed.length == 0u);
  assert(memo_encoder_begin(&encoder, buffer, sizeof(buffer)));
  phase = 0;
  uint32_t ring_phase = 0, out = 0;
  for (uint32_t f = 0; f < 50u; f++) {
    feed_tone(&encoder, 440.0, 3000.0, 321u, &phase);
    feed_tone(&ringed, 440.0, 3000.0, 321u, &ring_phase);
    while (memo_ring_pending(&ringed) >= 300u) {
      memo_ring_copy(&ringed, ringed.consumed, drained + out, 300u);
      out += 300u; ringed.consumed += 300u;
    }
  }
  assert(!ringed.full);
  uint32_t linear_length = memo_encoder_finish(&encoder, 0u);
  uint32_t ring_length = memo_encoder_finish(&ringed, 0u);
  uint32_t rest = memo_ring_pending(&ringed);
  memo_ring_copy(&ringed, ringed.consumed, drained + out, rest);
  out += rest;
  assert(ring_length == linear_length - MEMO_HEADER && out == ring_length && ring_length > 3u * sizeof(ring));
  assert(memcmp(drained, buffer + MEMO_HEADER, ring_length) == 0);
  assert(~ringed.crc == memo_crc32(drained, ring_length));
  uint8_t header[MEMO_HEADER];
  memo_write_header(&ringed, header, 0u);
  assert(memcmp(header, buffer, MEMO_HEADER) == 0);
  // A ring nobody drains fills up and stops cleanly.
  assert(memo_encoder_begin_ring(&ringed, ring, sizeof(ring)));
  for (int i = 0; i < 20; i++) memo_encoder_push(&ringed, loud, 320u);
  assert(ringed.full && memo_ring_pending(&ringed) == sizeof(ring));

  // Fixture for the Kotlin decoder: 1.5 s of 440 Hz, odd sample count included.
  static uint8_t fixture[MEMO_HEADER + 2u * MEMO_BYTES_PER_SECOND];
  assert(memo_encoder_begin(&encoder, fixture, sizeof(fixture)));
  phase = 0;
  feed_tone(&encoder, 440.0, 3000.0, 24003u, &phase);
  uint32_t fixture_length = memo_encoder_finish(&encoder, 42u);
  if (argc > 1) {
    FILE *file = fopen(argv[1], "wb");
    assert(file != NULL && fwrite(fixture, 1, fixture_length, file) == fixture_length && fclose(file) == 0);
  }
  // Watch speaker chain: quiet speech with DC and a loud click comes out loud, centered, unclipped.
  {
    enum { SECONDS = 4, PCM = 8000 * SECONDS, RAW = 2 * PCM };
    static int16_t raw[RAW];
    static uint8_t adpcm[MEMO_HEADER + PCM];
    static int8_t out[PCM + 200];
    for (int i = 0; i < RAW; i++) {
      double speech = 60.0 * sin(i * 2.0 * 3.14159265 * 220.0 / 16000.0);
      raw[i] = (int16_t)(50.0 + speech + (i > 32000 && i < 32040 ? (i % 2 ? 7000.0 : -7000.0) : 0.0));
    }
    MemoEncoder encoder;
    assert(memo_encoder_begin(&encoder, adpcm, sizeof(adpcm)));
    assert(memo_encoder_push(&encoder, raw, RAW));
    uint32_t file = memo_encoder_finish(&encoder, 1u);
    MemoPlayback playback;
    memset(&playback, 0, sizeof(playback));
    uint32_t head = 0u;
    for (uint32_t at = MEMO_HEADER; at < file; at += 256u) {
      memo_playback_feed(&playback, adpcm + at, file - at < 256u ? file - at : 256u, out, sizeof(out), &head);
    }
    memo_playback_flush(&playback, out, sizeof(out), &head);
    assert(head == (file - MEMO_HEADER) * 2u && head <= sizeof(out));
    for (uint32_t i = 0u; i < MEMO_PLAY_LEAD_MUTE; i++) assert(out[i] == 0); // Button press muted.
    int peak = 0, peak_after_click = 0;
    long sum = 0;
    for (uint32_t i = MEMO_PLAY_LEAD_MUTE; i < head; i++) {
      int v = out[i];
      sum += v;
      assert(v > -128 && v < 127); // The peak limit leaves headroom: nothing clips.
      if (abs(v) > peak) peak = abs(v);
      if (i > 16000u + 4000u && abs(v) > peak_after_click) peak_after_click = abs(v);
    }
    assert(peak >= 20);                                   // Raised well above the raw quarter-LSB.
    assert(peak_after_click >= 20);                       // A click does not duck the speech after it.
    assert(labs(sum) < (long)(head - MEMO_PLAY_LEAD_MUTE)); // DC removed.
  }

  printf("Voice memo codec: CRC-32, header, bounds, 440 Hz SNR %.1f dB, 7 kHz suppressed to %.0f RMS, ring mode, fixture %u bytes passed\n",
         snr, rms, (unsigned)fixture_length);
  return 0;
}
