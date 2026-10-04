// Exercise the production receiver and nonblocking PCM pump against a bounded speaker.
#include <pebble.h>
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

static uint8_t full_reply[HERMES_AUDIO_REPLY_MAX_BYTES];
static void full_message(uint8_t kind, uint32_t session, uint32_t total,
                         uint32_t offset, const uint8_t *bytes, uint16_t length) {
  audio_handle(kind, session, ++request_id, total, audio_checksum(full_reply, total),
               HERMES_AUDIO_REPLY_FORMAT, offset, bytes, length);
}
static void load_reply(uint32_t session, uint32_t length) {
  full_message(HERMES_KIND_AUDIO_BEGIN, session, length, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_READY);
  for (uint32_t offset = 0; offset < length; offset += 768) {
    uint16_t count = length - offset > 768 ? 768 : length - offset;
    full_message(HERMES_KIND_AUDIO_BLOCK, session, length, offset, full_reply + offset, count);
    assert(receipt.status == HERMES_AUDIO_BUFFERED && receipt.received == offset + count);
  }
}

int main(void) {
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

  for (uint32_t i = 0; i < sizeof(full_reply); i++) full_reply[i] = (uint8_t)(i * 43);
  storage_capacity = 4096;
  full_message(HERMES_KIND_AUDIO_BEGIN, 30, sizeof(full_reply), 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_STORAGE && s_audio.bytes == NULL);
  storage_capacity = 1024 * 1024;
  fail_storage_write = true;
  full_message(HERMES_KIND_AUDIO_BEGIN, 30, sizeof(full_reply), 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_STORAGE && s_audio.bytes == NULL);
  fail_storage_write = false;

  // One full minute is uploaded before a single speaker session starts.
  load_reply(31, sizeof(full_reply));
  assert(s_audio.received_checksum == audio_checksum(full_reply, sizeof(full_reply)));
  uint32_t full_opens = opens;
  uint32_t full_closes = closes;
  full_message(HERMES_KIND_AUDIO_BLOCK, 31, sizeof(full_reply), 0, full_reply, 768);
  assert(receipt.status == HERMES_AUDIO_BUFFERED && receipt.received == sizeof(full_reply));
  full_message(HERMES_KIND_AUDIO_BLOCK, 31, sizeof(full_reply), 1, full_reply, 768);
  assert(receipt.status == HERMES_AUDIO_INVALID);
  full_reply[0] ^= 1;
  full_message(HERMES_KIND_AUDIO_PLAY, 31, sizeof(full_reply), 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && opens == full_opens);
  full_reply[0] ^= 1;
  write_limit = 733; // Partial writes must resume within the cached read buffer.
  full_message(HERMES_KIND_AUDIO_PLAY, 31, sizeof(full_reply), 0, NULL, 0);
  for (int tick = 0; tick < 1300 && !s_audio.draining; tick++) audio_tick(NULL);
  assert(s_audio.draining && s_audio.phase == 2 && s_audio.bytes != NULL);
  assert(opens == full_opens + 1 && closes == full_closes + 1);
  assert(output_length == sizeof(full_reply) && memcmp(output, full_reply, sizeof(full_reply)) == 0);
  assert(s_audio.idle_ms > 10000); // No old two-second-clip timeout.
  device_status = SpeakerStatusIdle;
  finished(SpeakerFinishReasonDone, NULL);
  assert(receipt.status == HERMES_AUDIO_COMPLETE && s_audio.bytes == NULL);
  full_message(HERMES_KIND_AUDIO_PLAY, 31, sizeof(full_reply), 0, NULL, 0);
  assert(opens == full_opens + 1 && receipt.status == HERMES_AUDIO_COMPLETE);
  fail_storage_delete = true;
  audio_tick(NULL);
  assert(s_audio_cache_pages != 0 && persist_exists(AUDIO_CACHE_META));
  fail_storage_delete = false;
  for (int tick = 0; tick < 200 && s_audio_cache_pages != 0; tick++) audio_tick(NULL);
  assert(s_audio_cache_pages == 0 && !persist_exists(AUDIO_CACHE_META));
  for (uint32_t i = 0; i <= AUDIO_CACHE_MAX_PAGES; i++) assert(cache_sizes[i] == 0);

  // Odd final page/buffer lengths, read failures and Quiet Time never report success.
  load_reply(32, 24013);
  full_message(HERMES_KIND_AUDIO_PLAY, 32, 24013, 0, NULL, 0);
  for (int tick = 0; tick < 100 && !s_audio.draining; tick++) audio_tick(NULL);
  assert(s_audio.draining && output_length == 24013 && memcmp(output, full_reply, 24013) == 0);
  device_status = SpeakerStatusIdle; finished(SpeakerFinishReasonDone, NULL);
  load_reply(33, 24013); // Replaces the cache while cleanup is pending.
  fail_storage_read = true;
  full_message(HERMES_KIND_AUDIO_PLAY, 33, 24013, 0, NULL, 0);
  audio_tick(NULL);
  assert(s_audio.terminal_status == HERMES_AUDIO_STORAGE && s_audio.bytes == NULL);
  fail_storage_read = false;
  load_reply(34, 24013);
  full_message(HERMES_KIND_AUDIO_PLAY, 34, 24013, 0, NULL, 0);
  quiet = true; audio_tick(NULL); quiet = false;
  assert(s_audio.terminal_status == HERMES_AUDIO_QUIET_TIME && s_audio.bytes == NULL);

  full_message(HERMES_KIND_AUDIO_BEGIN, 35, 24013, 0, NULL, 0);
  fail_storage_write = true;
  full_message(HERMES_KIND_AUDIO_BLOCK, 35, 24013, 0, full_reply, 768);
  assert(receipt.status == HERMES_AUDIO_STORAGE && s_audio.received == 0);
  fail_storage_write = false;
  load_reply(36, 24013);
  full_message(HERMES_KIND_AUDIO_CANCEL, 36, 24013, 0, NULL, 0);
  assert(s_audio.terminal_status == HERMES_AUDIO_CANCELLED && s_audio.bytes == NULL);

  // Crash/relaunch: metadata is cleanup-only, never authorizes playback.
  load_reply(37, 24013);
  audio_shutdown();
  memset(&s_audio, 0, sizeof(s_audio));
  s_audio_cache_pages = 0;
  audio_cache_recover();
  full_opens = opens;
  assert(s_audio_cache_pages != 0);
  full_message(HERMES_KIND_AUDIO_PLAY, 37, 24013, 0, NULL, 0);
  assert(receipt.status == HERMES_AUDIO_INVALID && opens == full_opens);
  for (int tick = 0; tick < 200 && s_audio_cache_pages != 0; tick++) audio_tick(NULL);
  assert(!persist_exists(AUDIO_CACHE_META));
  uint8_t bad_marker = 255;
  persist_write_data(AUDIO_CACHE_META, &bad_marker, 1);
  audio_cache_recover();
  assert(s_audio_cache_pages == AUDIO_CACHE_MAX_PAGES);
  for (int tick = 0; tick < 200 && s_audio_cache_pages != 0; tick++) audio_tick(NULL);
  assert(!persist_exists(AUDIO_CACHE_META));
  puts("Full-reply cache, uninterrupted speaker session, partial writes, cleanup and restart checks passed");
  return 0;
}
