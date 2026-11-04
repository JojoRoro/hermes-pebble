#ifndef HERMES_AUDIO_ADPCM_H
#define HERMES_AUDIO_ADPCM_H
#include "memo_core.h" // Shares the IMA step tables.
// IMA ADPCM reference: https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf
#define AUDIO_STREAM_SAMPLES 1529u
#define AUDIO_RING_SIZE 24576u
#define AUDIO_PREBUFFER 12288u
static void audio_adpcm_decode(const uint8_t *packet, uint32_t samples, uint8_t *ring, uint32_t offset) {
  int32_t predictor = (int16_t)((uint16_t)packet[0] | ((uint16_t)packet[1] << 8));
  int index = packet[2];
  ring[offset % AUDIO_RING_SIZE] = (uint8_t)(predictor >> 8);
  for (uint32_t i = 1; i < samples; i++) {
    uint8_t code = (packet[4u + (i - 1u) / 2u] >> (((i - 1u) % 2u) * 4u)) & 15u;
    int32_t step = s_memo_steps[index];
    int32_t delta = step >> 3;
    if (code & 4u) delta += step;
    if (code & 2u) delta += step >> 1;
    if (code & 1u) delta += step >> 2;
    predictor += (code & 8u) ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    index += s_memo_index_steps[code & 7u];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    ring[(offset + i) % AUDIO_RING_SIZE] = (uint8_t)(predictor >> 8);
  }
}
#endif
