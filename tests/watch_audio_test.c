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

static bool muted, dictating, fail_open, block_writes, block_status;
static SpeakerStatus device_status;
static SpeakerFinishedCallback finished;
static uint32_t opens, stops, closes, output_length, status_count;
static uint8_t output[HERMES_AUDIO_MAX_BYTES];
static AudioReply receipt;

void app_log(uint8_t level, const char *file, int line, const char *format, ...) {
  (void)level; (void)file; (void)line; (void)format;
}

AppTimer *app_timer_register(uint32_t timeout, AppTimerCallback callback, void *data) {
  (void)timeout; (void)callback; (void)data; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; }
bool speaker_is_muted(void) { return muted; }
SpeakerStatus speaker_get_status(void) { return device_status; }
void speaker_set_finish_callback(SpeakerFinishedCallback callback, void *ctx) { (void)ctx; finished = callback; }
bool speaker_stream_open(SpeakerPcmFormat format, uint8_t volume) {
  assert(format == SpeakerPcmFormat_8kHz_8bit && volume == 80);
  if (fail_open) return false;
  opens++; output_length = 0; device_status = SpeakerStatusPlaying; return true;
}
uint32_t speaker_stream_write(const void *data, uint32_t count) {
  if (block_writes) return 0;
  if (count > 137) count = 137; // Force partial writes through the entire clip.
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

int main(void) {
  for (uint32_t i = 0; i < sizeof(fixture); i++) fixture[i] = (uint8_t)(i * 71);
  hash = audio_checksum(fixture, sizeof(fixture));
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
  finished(SpeakerFinishReasonDone, NULL);
  block_status = true;
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
  return 0;
}
