#ifndef HERMES_AUDIO_ADPCM_H
#define HERMES_AUDIO_ADPCM_H
// IMA ADPCM reference: https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf
#define AUDIO_STREAM_SAMPLES 1529u
#define AUDIO_RING_SIZE 24576u
#define AUDIO_PREBUFFER 12288u
static const int16_t s_adpcm_steps[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,
  66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,
  658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,
  3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
  16818,18500,20350,22385,24623,27086,29794,32767};
static const int8_t s_adpcm_indices[8] = {-1,-1,-1,-1,2,4,6,8};
static void audio_adpcm_decode(const uint8_t *packet, uint32_t samples, uint8_t *ring, uint32_t offset) {
  int32_t predictor = (int16_t)((uint16_t)packet[0] | ((uint16_t)packet[1] << 8));
  int index = packet[2];
  ring[offset % AUDIO_RING_SIZE] = (uint8_t)(predictor >> 8);
  for (uint32_t i = 1; i < samples; i++) {
    uint8_t code = (packet[4u + (i - 1u) / 2u] >> (((i - 1u) % 2u) * 4u)) & 15u;
    int32_t step = s_adpcm_steps[index];
    int32_t delta = step >> 3;
    if (code & 4u) delta += step;
    if (code & 2u) delta += step >> 1;
    if (code & 1u) delta += step >> 2;
    predictor += (code & 8u) ? -delta : delta;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    index += s_adpcm_indices[code & 7u];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    ring[(offset + i) % AUDIO_RING_SIZE] = (uint8_t)(predictor >> 8);
  }
}
#endif
