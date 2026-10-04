// Exercise the production receiver and nonblocking PCM pump against a bounded speaker.
#include <pebble.h>
#undef fopen
#undef fclose
#undef fread
#undef fgetc
#undef fputc
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include "../src/c/protocol.h"
#include "../src/c/audio_watch.h"

static bool muted, quiet, dictating, fail_open, block_writes, block_status;
static SpeakerStatus device_status;
static SpeakerFinishedCallback finished;
static uint32_t opens, stops, closes, output_length, status_count;
static uint8_t output[HERMES_AUDIO_REPLY_MAX_BYTES];
static AudioReply receipt;
static uint32_t write_limit = 137;
static uint8_t cache[AUDIO_CACHE_MAX_PAGES + 1][AUDIO_CACHE_PAGE];
static uint16_t cache_sizes[AUDIO_CACHE_MAX_PAGES + 1];
static bool fail_storage_write, fail_storage_read, fail_storage_delete;
static size_t storage_capacity = 1024 * 1024;

static uint32_t cache_index(uint32_t key) {
  assert(key >= AUDIO_CACHE_META && key <= AUDIO_CACHE_BASE + AUDIO_CACHE_MAX_PAGES - 1);
  return key - AUDIO_CACHE_META;
}
size_t persist_get_max_size(void) { return storage_capacity; }
bool persist_exists(uint32_t key) { return cache_sizes[cache_index(key)] != 0; }
int persist_write_data(uint32_t key, const void *data, size_t count) {
  assert(count <= AUDIO_CACHE_PAGE);
  if (fail_storage_write) return E_OUT_OF_STORAGE;
  uint32_t index = cache_index(key);
  memcpy(cache[index], data, count); cache_sizes[index] = count;
  return count;
}
int persist_read_data(uint32_t key, void *data, size_t count) {
  uint32_t index = cache_index(key);
  if (fail_storage_read || cache_sizes[index] == 0) return E_DOES_NOT_EXIST;
  if (count > cache_sizes[index]) count = cache_sizes[index];
  memcpy(data, cache[index], count); return count;
}
status_t persist_delete(uint32_t key) {
  if (fail_storage_delete) return E_ERROR;
  cache_sizes[cache_index(key)] = 0; return S_SUCCESS;
}

void app_log(uint8_t level, const char *file, int line, const char *format, ...) {
  (void)level; (void)file; (void)line; (void)format;
}

AppTimer *app_timer_register(uint32_t timeout, AppTimerCallback callback, void *data) {
  (void)timeout; (void)callback; (void)data; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; }
bool speaker_is_muted(void) { return muted; }
bool quiet_time_is_active(void) { return quiet; }
SpeakerStatus speaker_get_status(void) { return device_status; }
void speaker_set_finish_callback(SpeakerFinishedCallback callback, void *ctx) { (void)ctx; finished = callback; }
bool speaker_stream_open(SpeakerPcmFormat format, uint8_t volume) {
  assert(format == SpeakerPcmFormat_8kHz_8bit && volume == 80);
  if (fail_open) return false;
  opens++; output_length = 0; device_status = SpeakerStatusPlaying; return true;
}
uint32_t speaker_stream_write(const void *data, uint32_t count) {
  if (block_writes) return 0;
  if (count > write_limit) count = write_limit; // Force partial writes through the entire clip.
  assert(output_length + count <= sizeof(output));
  memcpy(output + output_length, data, count); output_length += count; return count;
}
void speaker_stream_close(void) { closes++; device_status = SpeakerStatusDraining; }
void speaker_stop(void) { stops++; device_status = SpeakerStatusIdle; }
static bool audio_dictation_active(void) { return dictating; }
static bool audio_send_status(uint32_t session, uint32_t total, uint32_t checksum,
                              uint32_t received, uint8_t status) {
  if (block_status) return false;
  receipt = (AudioReply){ .session = session, .total = total, .checksum = checksum,
    .received = received, .status = status, .request = s_audio_reply.request };
  status_count++; return true;
}

static uint8_t fixture[HERMES_AUDIO_MAX_BYTES];
static uint32_t request_id = 100;
static uint32_t hash;
static void message(uint8_t kind, uint32_t session, uint32_t offset, const uint8_t *data, uint16_t length) {
  audio_handle(kind, session, ++request_id, sizeof(fixture), hash, HERMES_AUDIO_FORMAT, offset, data, length);
  audio_tick(NULL);
}
static void load(uint32_t session) {
  message(HERMES_KIND_AUDIO_BEGIN, session, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_READY);
  for (uint32_t offset = 0; offset < sizeof(fixture); offset += 1000) {
    message(HERMES_KIND_AUDIO_BLOCK, session, offset, fixture + offset, 1000);
    assert(receipt.status == HERMES_AUDIO_BUFFERED && receipt.received == offset + 1000);
  }
}

static uint8_t packet[768];
static uint32_t stream_total, stream_hash;
static void stream_message(uint8_t kind, uint32_t session, uint32_t offset) {
  uint32_t samples = stream_total - offset;
  if (samples > AUDIO_STREAM_SAMPLES) samples = AUDIO_STREAM_SAMPLES;
  audio_handle(kind, session, ++request_id, stream_total, stream_hash,
    HERMES_AUDIO_STREAM_FORMAT, offset, kind == HERMES_KIND_AUDIO_BLOCK ? packet : NULL,
    kind == HERMES_KIND_AUDIO_BLOCK ? 4u + samples / 2u : 0u);
}
static void stream_setup(uint32_t total) {
  stream_total = total;
  stream_hash = 0x811c9dc5u;
  memset(packet, 0, sizeof(packet));
  for (uint32_t offset = 0; offset < total; offset += AUDIO_STREAM_SAMPLES) {
    uint32_t samples = total - offset;
    if (samples > AUDIO_STREAM_SAMPLES) samples = AUDIO_STREAM_SAMPLES;
    for (uint32_t i = 0; i < 4u + samples / 2u; i++) stream_hash = (stream_hash ^ packet[i]) * 0x01000193u;
  }
}

int main(int argc, char **argv) {
  if (argc == 4) {
    // Decode actual Kotlin packets through the production C decoder for a
    // cross-language comparison with an independent media decoder.
    FILE *input = fopen(argv[1], "rb"), *decoded = fopen(argv[2], "wb");
    assert(input && decoded);
    uint32_t total = (uint32_t)strtoul(argv[3], NULL, 10);
    uint8_t ring[AUDIO_RING_SIZE];
    for (uint32_t offset = 0; offset < total; offset += AUDIO_STREAM_SAMPLES) {
      uint32_t count = total - offset;
      if (count > AUDIO_STREAM_SAMPLES) count = AUDIO_STREAM_SAMPLES;
      uint32_t size = 4u + count / 2u;
      assert(fread(packet, 1, size, input) == size);
      audio_adpcm_decode(packet, count, ring, offset);
      for (uint32_t i = 0; i < count; i++) fputc(ring[(offset + i) % AUDIO_RING_SIZE], decoded);
    }
    assert(fgetc(input) == EOF);
    fclose(input); fclose(decoded); return 0;
  }

  for (uint32_t i = 0; i < sizeof(fixture); i++) fixture[i] = (uint8_t)(i * 71);
  hash = audio_checksum(fixture, sizeof(fixture));
  // Quiet Time blocks reception even if the system speaker mute preference is OFF.
  quiet = true;
  message(HERMES_KIND_AUDIO_BEGIN, 20, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME && s_audio.bytes == NULL && opens == 0);
  quiet = false;
  message(HERMES_KIND_AUDIO_BEGIN, 21, 0, NULL, 0);
  quiet = true;
  message(HERMES_KIND_AUDIO_BLOCK, 21, 0, fixture, 1000);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME && s_audio.bytes == NULL && s_audio.received == 0);
  quiet = false;
  message(HERMES_KIND_AUDIO_PLAY, 21, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME && opens == 0);
  load(22);
  quiet = true;
  message(HERMES_KIND_AUDIO_PLAY, 22, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME && s_audio.bytes == NULL && opens == 0);
  quiet = false;
  audio_shutdown();
  audio_handle(HERMES_KIND_AUDIO_BEGIN, 1, 1, HERMES_AUDIO_MAX_BYTES + 1, hash, 1, 0, NULL, 0);
  audio_tick(NULL);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.phase == 0);
  muted = true;
  message(HERMES_KIND_AUDIO_BEGIN, 1, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_MUTED && opens == 0);
  muted = false; dictating = true;
  message(HERMES_KIND_AUDIO_BEGIN, 1, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_BUSY);
  dictating = false;
  message(HERMES_KIND_AUDIO_BEGIN, 1, 0, NULL, 0);
  message(HERMES_KIND_AUDIO_BLOCK, 1, UINT32_MAX, fixture, 100);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.received == 0);
  message(HERMES_KIND_AUDIO_BLOCK, 2, 0, fixture, 100);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.received == 0);
  message(HERMES_KIND_AUDIO_PLAY, 1, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && opens == 0);
  load(3);
  message(HERMES_KIND_AUDIO_BLOCK, 3, 0, fixture, 1000);
  assert(receipt.status == HERMES_AUDIO_BUFFERED && s_audio.received == sizeof(fixture));
  uint8_t wrong = fixture[0] ^ 1;
  message(HERMES_KIND_AUDIO_BLOCK, 3, 0, &wrong, 1);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.bytes[0] == fixture[0]);
  s_audio.bytes[0] ^= 1;
  message(HERMES_KIND_AUDIO_PLAY, 3, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && opens == 0);
  s_audio.bytes[0] ^= 1;
  block_writes = true;
  uint32_t before = status_count;
  message(HERMES_KIND_AUDIO_PLAY, 3, 0, NULL, 0);
  assert(opens == 1 && s_audio.written == 0 && status_count == before);
  uint32_t play_request = request_id;
  block_writes = false;
  for (int i = 0; i < 150; i++) audio_tick(NULL);
  assert(closes == 1 && s_audio.draining && output_length == sizeof(fixture));
  assert(memcmp(output, fixture, sizeof(fixture)) == 0 && status_count == before);
  // Writing all bytes / closing the stream is still not playback completion.
  device_status = SpeakerStatusIdle;
  block_status = true;
  finished(SpeakerFinishReasonDone, NULL);
  audio_tick(NULL);
  assert(s_audio_reply.pending);
  block_status = false;
  audio_tick(NULL);
  assert(receipt.status == HERMES_AUDIO_COMPLETE && receipt.request == play_request);
  message(HERMES_KIND_AUDIO_PLAY, 3, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_COMPLETE && opens == 1);
  load(4);
  fail_open = true;
  message(HERMES_KIND_AUDIO_PLAY, 4, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_FAILED);
  fail_open = false;
  message(HERMES_KIND_AUDIO_PLAY, 4, 0, NULL, 0);
  finished(SpeakerFinishReasonPreempted, NULL);
  audio_tick(NULL);
  assert(receipt.status == HERMES_AUDIO_CANCELLED);
  load(5);
  message(HERMES_KIND_AUDIO_PLAY, 5, 0, NULL, 0);
  audio_shutdown();
  assert(stops > 0 && s_audio_timer == NULL && finished == NULL && !s_audio_reply.pending);
  message(HERMES_KIND_AUDIO_BEGIN, 6, 0, NULL, 0);
  for (int i = 0; i < 310; i++) audio_tick(NULL);
  assert(s_audio.phase == 3 && s_audio.terminal_status == HERMES_AUDIO_FAILED);
  load(23);
  quiet = true;
  audio_tick(NULL); // Discard a paused upload without waiting for another phone message.
  assert(s_audio.bytes == NULL && s_audio.terminal_status == HERMES_AUDIO_QUIET_TIME);
  quiet = false;
  load(24);
  message(HERMES_KIND_AUDIO_PLAY, 24, 0, NULL, 0);
  uint32_t previous_stops = stops;
  quiet = true;
  audio_tick(NULL);
  assert(stops == previous_stops + 1 && finished == NULL && s_audio.bytes == NULL);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME);
  quiet = false;
  message(HERMES_KIND_AUDIO_PLAY, 24, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_QUIET_TIME && stops == previous_stops + 1);
  audio_shutdown();

  // Full-length reply starts early, wraps a bounded ring repeatedly, and never
  // touches flash even if storage is unavailable. A faster sender is throttled.
  fail_storage_write = fail_storage_read = true;
  uint32_t full_opens = opens, full_closes = closes;
  stream_setup(HERMES_AUDIO_REPLY_MAX_BYTES - 1u); // odd final block
  stream_message(HERMES_KIND_AUDIO_BEGIN, 31, 0);
  assert(receipt.status == HERMES_AUDIO_READY);
  write_limit = 733;
  bool started = false, throttled = false;
  for (uint32_t offset = 0; offset < stream_total; offset += AUDIO_STREAM_SAMPLES) {
    stream_message(HERMES_KIND_AUDIO_BLOCK, 31, offset);
    for (int ticks = 0; receipt.status == HERMES_AUDIO_BUFFER_FULL && ticks < 100; ticks++) {
      throttled = true;
      assert(s_audio.received == offset);
      audio_tick(NULL);
      stream_message(HERMES_KIND_AUDIO_BLOCK, 31, offset);
    }
    assert(receipt.status == HERMES_AUDIO_BUFFERED);
    uint32_t received = s_audio.received;
    stream_message(HERMES_KIND_AUDIO_BLOCK, 31, offset); // retry cannot decode twice
    assert(s_audio.received == received);
    if (!started && received >= AUDIO_PREBUFFER) {
      assert(received < stream_total / 10u);
      stream_message(HERMES_KIND_AUDIO_PLAY, 31, 0);
      started = true;
    }
    assert(s_audio.received - s_audio.written <= AUDIO_RING_SIZE);

    for (int ticks = 0; s_audio_reply.pending && ticks < 100; ticks++) audio_tick(NULL);
    assert(!s_audio_reply.pending && receipt.status == HERMES_AUDIO_BUFFERED);
  }
  for (int ticks = 0; !s_audio.draining && ticks < 200; ticks++) audio_tick(NULL);
  assert(throttled && s_audio.draining && output_length == stream_total);
  for (uint32_t i = 0; i < output_length; i++) assert(output[i] == 0);
  assert(opens == full_opens + 1 && closes == full_closes + 1);
  device_status = SpeakerStatusIdle; finished(SpeakerFinishReasonDone, NULL);
  assert(receipt.status == HERMES_AUDIO_COMPLETE && s_audio.bytes == NULL);
  stream_message(HERMES_KIND_AUDIO_PLAY, 31, 0);
  assert(opens == full_opens + 1 && receipt.status == HERMES_AUDIO_COMPLETE);

  // Slow sender: preserve the stream and wait for a useful buffer to accumulate.
  stream_setup(32000);
  stream_message(HERMES_KIND_AUDIO_BEGIN, 32, 0);
  uint32_t offset = 0;
  while (offset < AUDIO_PREBUFFER) {
    stream_message(HERMES_KIND_AUDIO_BLOCK, 32, offset); offset += AUDIO_STREAM_SAMPLES;
  }
  stream_message(HERMES_KIND_AUDIO_PLAY, 32, 0);
  for (int ticks = 0; ticks < 40; ticks++) audio_tick(NULL);
  assert(s_audio.rebuffering && !s_audio.draining && s_audio.phase == 2);
  uint32_t written = s_audio.written;
  stream_message(HERMES_KIND_AUDIO_BLOCK, 32, offset); offset += AUDIO_STREAM_SAMPLES;
  audio_tick(NULL);
  assert(s_audio.written == written); // Don't stutter on every tiny new block.
  while (offset < stream_total) {
    stream_message(HERMES_KIND_AUDIO_BLOCK, 32, offset); offset += AUDIO_STREAM_SAMPLES;
    audio_tick(NULL);
  }
  for (int ticks = 0; ticks < 100 && !s_audio.draining; ticks++) audio_tick(NULL);
  assert(s_audio.draining && output_length == stream_total);
  device_status = SpeakerStatusIdle; finished(SpeakerFinishReasonDone, NULL);

  // Back/Quiet Time and a stalled sender stop and release the entire RAM ring.
  for (uint32_t session = 33; session < 36; session++) {
    stream_setup(32000); stream_message(HERMES_KIND_AUDIO_BEGIN, session, 0);
    for (offset = 0; offset < AUDIO_PREBUFFER; offset += AUDIO_STREAM_SAMPLES)
      stream_message(HERMES_KIND_AUDIO_BLOCK, session, offset);
    stream_message(HERMES_KIND_AUDIO_PLAY, session, 0);
    if (session == 33) stream_message(HERMES_KIND_AUDIO_CANCEL, session, 0);
    if (session == 34) { quiet = true; audio_tick(NULL); quiet = false; }
    if (session == 35) for (int ticks = 0; ticks < 700; ticks++) audio_tick(NULL);
    assert(s_audio.phase == 3 && s_audio.bytes == NULL);
    assert(s_audio.terminal_status == (session == 33 ? HERMES_AUDIO_CANCELLED :
      session == 34 ? HERMES_AUDIO_QUIET_TIME : HERMES_AUDIO_FAILED));
  }
  // Reject malformed codec headers, gaps, changed retries, and checksum failure.
  stream_setup(32000); stream_message(HERMES_KIND_AUDIO_BEGIN, 36, 0);
  packet[2] = 89; stream_message(HERMES_KIND_AUDIO_BLOCK, 36, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.received == 0);
  packet[2] = 0; stream_message(HERMES_KIND_AUDIO_BLOCK, 36, AUDIO_STREAM_SAMPLES);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.received == 0);
  stream_message(HERMES_KIND_AUDIO_BLOCK, 36, 0);
  packet[4] = 1; stream_message(HERMES_KIND_AUDIO_BLOCK, 36, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.received == AUDIO_STREAM_SAMPLES);
  stream_message(HERMES_KIND_AUDIO_CANCEL, 36, 0);
  stream_setup(2); stream_hash ^= 1;
  stream_message(HERMES_KIND_AUDIO_BEGIN, 37, 0);
  stream_message(HERMES_KIND_AUDIO_BLOCK, 37, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && s_audio.bytes == NULL);

  // Migration removes at most one old flash page per idle tick, never replays it.
  fail_storage_write = fail_storage_read = false;
  uint32_t old_pages = 3;
  persist_write_data(AUDIO_CACHE_META, &old_pages, sizeof(old_pages));
  persist_write_data(AUDIO_CACHE_BASE + 2, packet, 20);
  audio_cache_recover();
  fail_storage_delete = true; audio_tick(NULL);
  assert(s_audio_cache_pages == 3);
  fail_storage_delete = false; audio_tick(NULL);
  assert(s_audio_cache_pages == 2);
  audio_tick(NULL); audio_tick(NULL);
  assert(!persist_exists(AUDIO_CACHE_META));
  puts("Streaming start, backpressure, wraparound, underrun recovery, cancellation and migration checks passed");
  return 0;
}
