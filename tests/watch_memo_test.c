// Drive the production voice memo state machine (memo_watch.h) against a simulated
// microphone, outbox and phone. Uses tests/memo_stub/pebble.h, not the SDK.
#include <pebble.h>
#include <assert.h>
#include <math.h>
#include "../src/c/protocol.h"

static uint32_t read_le32(const uint8_t *b, uint32_t o) {
  return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

#define UI_HEADER_HEIGHT 24
#define UI_HINT_HEIGHT 20
#define UI_MARGIN 8
#define UI_ACCENT GColorCobaltBlue

typedef struct {
  uint32_t capture_id, generation, total_bytes;
  uint8_t status, error_code;
} InboundTransfer;
static struct { bool active; } s_outbound;
static bool s_exiting, s_app_message_open = true;
static uint32_t s_capture_counter = 100u, s_transfer_counter = 5000u;
static bool storage_next_capture_id(uint32_t *capture_id) { *capture_id = ++s_capture_counter; return true; }
static uint32_t next_transfer_id(void) { return ++s_transfer_counter; }

struct DictionaryIterator {
  uint32_t kind, transfer, capture, offset, total, crc, flags, version;
  uint8_t payload[1024];
  uint16_t length;
};
static bool read_u32(DictionaryIterator *iter, uint8_t key, uint32_t *value, bool required) {
  (void)required;
  if (key != HERMES_KEY_TRANSFER_ID) return false;
  *value = iter->transfer;
  return true;
}

#include "../src/c/memo_watch.h"
static uint32_t speaker_stops;
static void memo_quiet_speaker(void) { speaker_stops++; }

// ---- Simulated platform -------------------------------------------------------------
// The firmware side of mic_shim.h: a function table reached through the app header.
static MemoFirmwareMicHandlers mic;
static bool mic_active;
static MicCaptureStartResult mic_result = MicCaptureStartOk;
static void *firmware_table[683];
const PebbleProcessInfo __pbl_app_info = {
  .header = "PBLAPP", .sym_table_addr = offsetof(PebbleProcessInfo, loader_table), .loader_table = firmware_table,
};
#ifdef MEMO_TEST_OLD_FIRMWARE
static WatchInfoVersion firmware = { 4, 42, 0 };
#else
static WatchInfoVersion firmware = { 4, 43, 0 };
#endif
WatchInfoVersion watch_info_get_firmware_version(void) { return firmware; }
static uint32_t firmware_mic_start(MemoFirmwareMicHandlers handlers, void *context) {
  (void)context;
  assert(handlers.data && handlers.stopped && !mic_active);
  if (mic_result != MicCaptureStartOk) return 0xa5a5a500u | mic_result; // Short enum: upper bits are junk.
  mic = handlers;
  mic_active = true;
  return 0x12345600u;
}
static void firmware_mic_stop(void) { mic_active = false; }
static size_t free_heap = 100000u;
static bool connected = true, outbox_busy, in_flight;
static uint32_t outbox_limit = 1024u, sends, vibes;
static DictionaryIterator dict;
static AppTimerCallback timer_callback;
static uint32_t timer_ms;
static int timers_registered;

void layer_mark_dirty(Layer *layer) { (void)layer; }
Layer *layer_create(GRect frame) { (void)frame; return (Layer *)malloc(1); }
void layer_destroy(Layer *layer) { free(layer); }
void layer_set_update_proc(Layer *layer, LayerUpdateProc proc) { (void)layer; (void)proc; }
void layer_add_child(Layer *parent, Layer *child) { (void)parent; (void)child; }
GRect layer_get_bounds(const Layer *layer) { (void)layer; return GRect(0, 0, 200, 228); }
Window *window_create(void) { return (Window *)malloc(1); }
void window_destroy(Window *window) { free(window); }
Layer *window_get_root_layer(const Window *window) { (void)window; return NULL; }
void window_set_click_config_provider(Window *window, ClickConfigProvider provider) { (void)window; (void)provider; }
void window_set_window_handlers(Window *window, WindowHandlers handlers) { (void)window; (void)handlers; }
void window_stack_push(Window *window, bool animated) { (void)window; (void)animated; }
Window *window_stack_pop(bool animated) { (void)animated; return NULL; }
void window_single_click_subscribe(ButtonId button, ClickHandler handler) { (void)button; (void)handler; }
AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback, void *data) {
  (void)data; timer_callback = callback; timer_ms = timeout_ms; timers_registered++; return (AppTimer *)1;
}
void app_timer_cancel(AppTimer *timer) { (void)timer; timer_callback = NULL; }
size_t heap_bytes_free(void) { return free_heap; }
// Persisted memo mode.
static int32_t persisted_mode = -1;
int32_t persist_read_int(uint32_t key) { assert(key == HERMES_STORAGE_KEY_MEMO_MODE); return persisted_mode < 0 ? 0 : persisted_mode; }
int persist_write_int(uint32_t key, int32_t value) { assert(key == HERMES_STORAGE_KEY_MEMO_MODE); persisted_mode = value; return 4; }

// Data Logging: one session, items checked and stored like the phone would get them.
static bool log_open, log_create_fails;
static uint32_t log_calls, log_busy_every, log_finishes, log_capture;
static DataLoggingResult log_result = DATA_LOGGING_SUCCESS;
static uint8_t log_data[600000];
static uint32_t log_length;
static uint8_t log_end[MEMO_LOG_ITEM];
static bool log_end_seen;
DataLoggingSessionRef data_logging_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_length, bool resume) {
  assert(tag == MEMO_LOG_TAG && item_type == DATA_LOGGING_BYTE_ARRAY && item_length == MEMO_LOG_ITEM && !resume && !log_open);
  if (log_create_fails) return NULL;
  log_open = true;
  log_length = 0u;
  log_end_seen = false;
  return (DataLoggingSessionRef)&log_open;
}
DataLoggingResult data_logging_log(DataLoggingSessionRef session, const void *data, uint32_t num_items) {
  assert(session == (DataLoggingSessionRef)&log_open && log_open && num_items == 1u && !log_end_seen);
  log_calls++;
  if (log_result != DATA_LOGGING_SUCCESS) return log_result;
  if (log_busy_every && log_calls % log_busy_every == 0u) return DATA_LOGGING_BUSY; // Flash write pending.
  const uint8_t *item = data;
  uint32_t capture = read_le32(item, 0u), offset = read_le32(item, 4u);
  uint16_t length = (uint16_t)(item[10] | item[11] << 8);
  log_capture = capture;
  assert(offset == log_length && length <= MEMO_LOG_PAYLOAD);
  if (item[8] == 1u) {
    assert(length == MEMO_HEADER + 8u);
    memcpy(log_end, item, MEMO_LOG_ITEM);
    log_end_seen = true;
  } else {
    assert(item[8] == 0u && length > 0u);
    memcpy(log_data + offset, item + MEMO_LOG_HEAD, length);
    log_length += length;
  }
  return DATA_LOGGING_SUCCESS;
}
void data_logging_finish(DataLoggingSessionRef session) {
  assert(session == (DataLoggingSessionRef)&log_open && log_open);
  log_open = false;
  log_finishes++;
}

// Wall clock for coverage: speak() advances it by each frame's 20 ms (or more when frames drop).
static uint64_t clock_ms = 1700000000000ull;
uint16_t time_ms(time_t *tloc, uint16_t *out_ms) {
  if (tloc) *tloc = (time_t)(clock_ms / 1000u);
  if (out_ms) *out_ms = (uint16_t)(clock_ms % 1000u);
  return (uint16_t)(clock_ms % 1000u);
}
void vibes_short_pulse(void) { vibes++; }
bool connection_service_peek_pebble_app_connection(void) { return connected; }
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator) {
  if (outbox_busy || in_flight) return APP_MSG_BUSY;
  memset(&dict, 0, sizeof(dict));
  *iterator = &dict;
  return APP_MSG_OK;
}
AppMessageResult app_message_outbox_send(void) { in_flight = true; sends++; return APP_MSG_OK; }
DictionaryResult dict_write_uint8(DictionaryIterator *iter, uint32_t key, uint8_t value) {
  if (key == HERMES_KEY_MESSAGE_KIND) iter->kind = value;
  if (key == HERMES_KEY_FLAGS) iter->flags = value;
  if (key == HERMES_KEY_PROTOCOL_VERSION) iter->version = value;
  return DICT_OK;
}
DictionaryResult dict_write_uint32(DictionaryIterator *iter, uint32_t key, uint32_t value) {
  if (key == HERMES_KEY_TRANSFER_ID) iter->transfer = value;
  if (key == HERMES_KEY_CAPTURE_ID) iter->capture = value;
  if (key == HERMES_KEY_PAGE_OFFSET) iter->offset = value;
  if (key == HERMES_KEY_TOTAL_BYTES) iter->total = value;
  if (key == HERMES_KEY_CONVERSATION_GENERATION) iter->crc = value;
  return DICT_OK;
}
DictionaryResult dict_write_data(DictionaryIterator *iter, uint32_t key, const uint8_t *data, uint16_t size) {
  assert(key == HERMES_KEY_PAYLOAD);
  // Header and eight other tuples take about 80 bytes of the 1,024-byte outbox.
  if (size + 80u > outbox_limit) return DICT_NOT_ENOUGH_STORAGE;
  memcpy(iter->payload, data, size);
  iter->length = size;
  return DICT_OK;
}
GFont fonts_get_system_font(const char *key) { return (GFont)key; }
void graphics_context_set_fill_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_stroke_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_context_set_text_color(GContext *ctx, GColor color) { (void)ctx; (void)color; }
void graphics_fill_rect(GContext *ctx, GRect rect, uint16_t radius, GCornerMask corners) { (void)ctx; (void)rect; (void)radius; (void)corners; }
void graphics_fill_circle(GContext *ctx, GPoint p, uint16_t radius) { (void)ctx; (void)p; (void)radius; }
void graphics_draw_round_rect(GContext *ctx, GRect rect, uint16_t radius) { (void)ctx; (void)rect; (void)radius; }
void graphics_draw_text(GContext *ctx, const char *text, GFont font, GRect box, GTextOverflowMode mode,
                        GTextAlignment alignment, void *attributes) {
  (void)ctx; (void)font; (void)box; (void)mode; (void)alignment; (void)attributes;
  assert(text != NULL && strlen(text) < 200u);
}

// ---- Simulated phone ----------------------------------------------------------------
static uint8_t phone[200000];
static uint32_t phone_length;

static uint8_t stream[600000], stream_header[MEMO_HEADER];
static uint32_t stream_length, stream_crc, stream_total;
static bool stream_end;

/* The phone stores the in-flight block and ACKs it; returns the block size. */
static uint16_t phone_ack(void) {
  assert(in_flight && dict.version == HERMES_PROTOCOL_VERSION);
  if (dict.kind == HERMES_KIND_MEMO_STREAM) {
    assert(dict.offset == stream_length && !stream_end); // In order, nothing skipped or repeated.
    if (dict.flags & HERMES_FLAG_MEMO_END) {
      assert(dict.length == MEMO_HEADER && dict.total == stream_length);
      memcpy(stream_header, dict.payload, MEMO_HEADER);
      stream_crc = dict.crc;
      stream_total = dict.total;
      stream_end = true;
    } else {
      assert(dict.flags == HERMES_FLAG_MORE && dict.length > 0u && dict.total == 0u);
      memcpy(stream + dict.offset, dict.payload, dict.length);
      stream_length += dict.length;
    }
  } else {
    assert(dict.kind == HERMES_KIND_MEMO_BLOCK && dict.offset <= phone_length);
    memcpy(phone + dict.offset, dict.payload, dict.length);
    if (dict.offset + dict.length > phone_length) phone_length = dict.offset + dict.length;
    assert(((dict.flags & HERMES_FLAG_MORE) != 0) == (dict.offset + dict.length < dict.total));
  }
  in_flight = false;
  uint16_t length = dict.length;
  assert(memo_on_sent(&dict));
  return length;
}

static void phone_nack(void) { assert(in_flight); in_flight = false; assert(memo_on_failed(&dict)); }

static InboundTransfer receipt(bool ok) {
  return (InboundTransfer){ .capture_id = s_memo.capture, .generation = s_memo.crc, .total_bytes = s_memo.length,
    .status = ok ? HERMES_STATUS_NOTE_SAVED : HERMES_STATUS_FAILED, .error_code = ok ? 0u : HERMES_ERROR_MALFORMED };
}

static void fire_timer(void) {
  assert(timer_callback != NULL);
  AppTimerCallback callback = timer_callback;
  timer_callback = NULL;
  callback(NULL);
}

/* Speech-like test audio; the same phase reproduces the same frames. */
static uint32_t phase;
static void make_frame(int16_t *frame, uint32_t *at, int16_t dc) {
  for (int i = 0; i < 320; i++, (*at)++) frame[i] = (int16_t)(dc + 2000.0 * sin(*at * 0.17) * (1.0 + sin(*at * 0.0011)));
}

/* Delivers one frame per frame_ms of wall clock: 20 ms is real time, more means drops. */
static void speak_paced(uint32_t frames, uint32_t frame_ms, int16_t dc) {
  int16_t frame[320];
  for (uint32_t f = 0; f < frames && s_memo.state == MEMO_RECORDING; f++) {
    make_frame(frame, &phase, dc);
    clock_ms += frame_ms;
    mic.data(frame, 320u, NULL);
  }
}

/* Live modes: after each frame the phone acknowledges (stream) unless it is lagging. */
static void speak_live(uint32_t frames, bool acking) {
  int16_t frame[320];
  for (uint32_t f = 0; f < frames && s_memo.state == MEMO_RECORDING; f++) {
    make_frame(frame, &phase, 0);
    clock_ms += 20u;
    mic.data(frame, 320u, NULL);
    if (acking && in_flight) phone_ack();
  }
}

/* The ADPCM a RAM-mode memo would hold for the same frames. */
static uint32_t reference(uint32_t from_phase, uint32_t frames, uint8_t *out, uint32_t capacity) {
  MemoEncoder ref;
  int16_t frame[320];
  assert(memo_encoder_begin(&ref, out, capacity));
  for (uint32_t f = 0; f < frames; f++) { make_frame(frame, &from_phase, 0); memo_encoder_push(&ref, frame, 320u); }
  return memo_encoder_finish(&ref, 0u) - MEMO_HEADER;
}
static void speak(uint32_t frames) { speak_paced(frames, 20u, 0); }

static void draw(void) { memo_draw(NULL, NULL); }

static void record(uint32_t frames) {
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_RECORDING && mic_active);
  draw();
  speak(frames);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_READY && !mic_active && s_memo.buffer && s_memo.length > MEMO_HEADER);
  assert(s_memo.crc == memo_crc32(s_memo.buffer, s_memo.length));
  draw();
}

int main(void) {
  firmware_table[MEMO_MIC_SLOT_START] = (void *)firmware_mic_start;
  firmware_table[MEMO_MIC_SLOT_STOP] = (void *)firmware_mic_stop;
#ifdef MEMO_TEST_OLD_FIRMWARE
  // Firmware 4.42's table ends before these slots: never touch them, explain instead.
  memo_open();
  memo_load(NULL);
  draw();
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !mic_active && !s_memo.buffer && strstr(s_memo.status, "4.43"));
  memo_shutdown();
  printf("Voice memo on firmware 4.42: microphone slots untouched, update message shown\n");
  return 0;
#endif
  assert(memo_mic_available());
  memo_open();
  memo_load(NULL);
  draw();

  // Happy path: 3 s of speech, ACK-paced 768-byte blocks, receipt frees the clip.
  record(150u);
  assert(speaker_stops == 1u); // Reply playback is stopped before the microphone opens.
  assert(s_memo.encoder.samples == 150u * 160u - 3u);
  assert(s_memo.encoder.coverage >= 99u && s_memo.buffer[7] == s_memo.encoder.coverage);
  assert(strstr(s_memo.status, "missed") == NULL);
  uint32_t expected = s_memo.length, crc = s_memo.crc, capture = s_memo.capture;
  static uint8_t original[200000];
  memcpy(original, s_memo.buffer, expected);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING && in_flight && dict.offset == 0u && dict.length == MEMO_BLOCK_BYTES);
  assert(dict.capture == capture && dict.crc == crc && dict.total == expected);
  assert(strcmp(memo_subtitle(), "Sending… 0%") == 0);
  uint32_t blocks = 0;
  while (s_memo.state == MEMO_SENDING) { phone_ack(); blocks++; draw(); }
  assert(blocks == (expected + MEMO_BLOCK_BYTES - 1u) / MEMO_BLOCK_BYTES);
  assert(s_memo.state == MEMO_WAIT_RECEIPT && phone_length == expected && memcmp(phone, original, expected) == 0);
  InboundTransfer wrong = receipt(true);
  wrong.generation ^= 1u;
  memo_receipt(&wrong);
  assert(s_memo.state == MEMO_WAIT_RECEIPT);
  InboundTransfer ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT && s_memo.buffer == NULL && vibes == 1u);
  draw();
  memo_receipt(&ok); // Duplicate receipt is harmless.
  assert(s_memo.state == MEMO_SENT);

  // A missing receipt resends the final block; the phone's repeated receipt completes it.
  memo_select(NULL, NULL);
  speak(60u);
  memo_select(NULL, NULL);
  assert(s_memo.capture == capture + 1u);
  memo_select(NULL, NULL);
  phone_length = 0u;
  uint32_t final_offset = 0u;
  while (s_memo.state == MEMO_SENDING) { final_offset = dict.offset; phone_ack(); }
  assert(s_memo.state == MEMO_WAIT_RECEIPT && timer_ms == MEMO_RECEIPT_TIMEOUT_MS);
  fire_timer();
  assert(s_memo.state == MEMO_SENDING && in_flight && dict.offset == final_offset);
  phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT);

  // A receipt that overtakes the final ACK completes the memo; the late ACK is ignored.
  memo_select(NULL, NULL);
  speak(30u);
  memo_select(NULL, NULL);
  memo_select(NULL, NULL);
  while (dict.offset + dict.length < dict.total) phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT && !s_memo.in_flight);
  in_flight = false;
  assert(!memo_on_sent(&dict));

  // NACKs back off, shrink blocks after repeated refusals, then give up with the clip kept.
  memo_select(NULL, NULL);
  speak(100u);
  memo_select(NULL, NULL);
  memo_select(NULL, NULL);
  phone_nack();
  assert(s_memo.block == MEMO_BLOCK_BYTES && timer_ms == 500u);
  fire_timer();
  phone_nack();
  assert(s_memo.block == MEMO_BLOCK_BYTES / 2u);
  fire_timer();
  assert(in_flight && dict.length == MEMO_BLOCK_BYTES / 2u && dict.offset == 0u);
  phone_ack();
  assert(s_memo.retries == 0u && s_memo.offset == MEMO_BLOCK_BYTES / 2u);
  for (int i = 0; i <= (int)MEMO_MAX_RETRIES; i++) { phone_nack(); if (s_memo.state == MEMO_SENDING) fire_timer(); }
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer != NULL && s_memo.block == MEMO_MIN_BLOCK_BYTES);
  draw();
  // Retry restarts from zero at full block size; a checksum rejection is reported.
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING && dict.offset == 0u && dict.length == MEMO_BLOCK_BYTES);
  while (s_memo.state == MEMO_SENDING) phone_ack();
  InboundTransfer bad = receipt(false);
  memo_receipt(&bad);
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer != NULL);
  // Delete needs a confirming second press; UP cancels.
  memo_down(NULL, NULL);
  assert(s_memo.confirm_delete && s_memo.buffer);
  draw();
  memo_up(NULL, NULL);
  assert(!s_memo.confirm_delete && s_memo.buffer);
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && s_memo.buffer == NULL);

  // The main transport has priority; a busy outbox and a small host outbox are tolerated.
  record(40u);
  s_outbound.active = true;
  memo_select(NULL, NULL);
  assert(!in_flight && timer_ms == MEMO_YIELD_MS);
  s_outbound.active = false;
  outbox_busy = true;
  fire_timer();
  assert(!in_flight && timer_ms == MEMO_YIELD_MS);
  outbox_busy = false;
  outbox_limit = 512u;
  fire_timer();
  assert(!in_flight && s_memo.block == MEMO_BLOCK_BYTES / 2u);
  fire_timer();
  assert(in_flight && dict.length == 384u);
  while (s_memo.state == MEMO_SENDING) phone_ack();
  ok = receipt(true);
  memo_receipt(&ok);
  assert(s_memo.state == MEMO_SENT);
  outbox_limit = 1024u;

  // Dropped frames: only one in five arrives, so the memo says it has gaps.
  memo_select(NULL, NULL);
  speak_paced(50u, 100u, 0);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_READY && s_memo.encoder.coverage >= 19u && s_memo.encoder.coverage <= 20u);
  assert(s_memo.buffer[7] == s_memo.encoder.coverage && strstr(s_memo.status, "missed audio: only"));
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // The level meter ignores a DC offset: silence at +12000 reads empty, speech reads high.
  memo_select(NULL, NULL);
  int16_t still[320];
  for (int i = 0; i < 320; i++) still[i] = 12000;
  for (int i = 0; i < 20; i++) mic.data(still, 320u, NULL);
  assert(s_memo.level == 0u);
  speak_paced(5u, 20u, 12000);
  assert(s_memo.level >= 8u);
  memo_down(NULL, NULL); // Ignored while recording.
  memo_select(NULL, NULL);
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE);

  // Disconnected: fail fast, keep the clip.
  record(40u);
  connected = false;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_FAILED && s_memo.buffer);
  connected = true;
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // Microphone refusals leave nothing allocated and explain why.
  mic_result = MicCaptureStartErrPermissionDenied;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "Microphone not allowed"));
  mic_result = MicCaptureStartOk;

  // Memory: the buffer follows free heap; too little refuses; a full buffer stops itself.
  free_heap = MEMO_HEAP_RESERVE + 1000u;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "memory"));
  free_heap = MEMO_HEAP_RESERVE + MEMO_HEADER + 4u * MEMO_BYTES_PER_SECOND;
  memo_select(NULL, NULL);
  assert(s_memo.capacity == MEMO_HEADER + 4u * MEMO_BYTES_PER_SECOND);
  speak(1000u);
  assert(s_memo.state == MEMO_READY && s_memo.length == s_memo.capacity && !mic_active && vibes == 5u);
  assert(memo_encoder_seconds(&s_memo.encoder) == 4u);
  free_heap = 100000u;
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);

  // Interruptions keep what was recorded; revocation deletes it; too-short clips are dropped.
  memo_select(NULL, NULL);
  speak(50u);
  mic_active = false;
  mic.stopped(0xffffff00u | MicCaptureStopReasonInterrupted, NULL);
  assert(s_memo.state == MEMO_READY && s_memo.buffer && strstr(s_memo.status, "interrupted"));
  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  memo_select(NULL, NULL);
  speak(50u);
  mic_active = false;
  mic.stopped(MicCaptureStopReasonPermissionRevoked, NULL);
  mic.stopped(MicCaptureStopReasonFocusLost, NULL); // A late callback after the clip is gone is ignored.
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer);
  memo_select(NULL, NULL);
  speak(10u);
  memo_back(NULL, NULL); // Back while recording stops instead of leaving.
  assert(s_memo.state == MEMO_IDLE && !s_memo.buffer && strstr(s_memo.status, "Too short"));
  memo_select(NULL, NULL);
  speak(50u);
  memo_disappear(NULL);
  assert(s_memo.state == MEMO_READY && !mic_active);

  memo_down(NULL, NULL);
  memo_down(NULL, NULL);
  static uint8_t expect[600000];

  // UP cycles RAM -> Stream -> Watch flash -> RAM and remembers the choice.
  assert(s_memo.mode == MEMO_MODE_RAM && s_memo.state == MEMO_IDLE);
  memo_up(NULL, NULL);
  assert(s_memo.mode == MEMO_MODE_STREAM && persisted_mode == (int32_t)MEMO_MODE_STREAM);
  draw();

  // Stream needs the phone at the start.
  connected = false;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !mic_active && !s_memo.buffer && strstr(s_memo.status, "needs the phone"));
  connected = true;

  // Stream: 20 s through an 8 KiB ring (it wraps five times), acknowledged as it goes.
  free_heap = MEMO_HEAP_RESERVE + 8192u;
  uint32_t start = phase;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_RECORDING && s_memo.capacity == 8192u && s_memo.encoder.ring);
  uint32_t stream_capture = s_memo.capture;
  speak_live(1000u, true);
  draw();
  assert(stream_length > 0u && stream_length + 2u * MEMO_BLOCK_BYTES >= s_memo.encoder.length);
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING && !mic_active);
  while (s_memo.state == MEMO_SENDING) { assert(in_flight); phone_ack(); }
  assert(s_memo.state == MEMO_WAIT_RECEIPT && stream_end);
  uint32_t data_length = reference(start, 1000u, expect, sizeof(expect));
  assert(stream_length == data_length && stream_total == data_length && data_length > 8u * 8192u);
  assert(memcmp(stream, expect + MEMO_HEADER, data_length) == 0);
  assert(stream_crc == memo_crc32(stream, stream_length) && stream_crc == s_memo.crc);
  assert(memcmp(stream_header, "HVM1", 4) == 0 && (read_le32(stream_header, 8) + 1u) / 2u == stream_length);
  assert(stream_header[7] >= 99u); // Coverage measured.
  InboundTransfer streamed = { .capture_id = stream_capture, .generation = stream_crc, .total_bytes = stream_length,
    .status = HERMES_STATUS_NOTE_SAVED };
  memo_receipt(&streamed);
  assert(s_memo.state == MEMO_SENT && !s_memo.buffer);

  // Stream while the phone lags: nothing is acknowledged, the ring fills, recording stops,
  // and the rest still goes out, in order, once the phone catches up.
  stream_length = 0u; stream_end = false;
  start = phase;
  memo_select(NULL, NULL);
  speak_live(1000u, false);
  assert(s_memo.state == MEMO_SENDING && !mic_active && strstr(s_memo.status, "fell behind"));
  // The ring filled part-way through a frame: compare all but the last (half-filled) byte.
  uint32_t lag_frames = (s_memo.encoder.samples + 3u + 159u) / 160u;
  assert(s_memo.length >= 8192u && s_memo.length <= 8192u + MEMO_BLOCK_BYTES);
  while (s_memo.state == MEMO_SENDING) { if (!in_flight) fire_timer(); else phone_ack(); }
  data_length = reference(start, lag_frames, expect, sizeof(expect));
  assert(stream_length == s_memo.length && data_length >= stream_length);
  assert(memcmp(stream, expect + MEMO_HEADER, stream_length - 1u) == 0 && stream_crc == memo_crc32(stream, stream_length));
  streamed = (InboundTransfer){ .capture_id = s_memo.capture, .generation = s_memo.crc, .total_bytes = s_memo.length,
    .status = HERMES_STATUS_NOTE_SAVED };
  memo_receipt(&streamed);
  assert(s_memo.state == MEMO_SENT);

  // Stream through a short Bluetooth outage: recording continues and catches up afterwards.
  stream_length = 0u; stream_end = false;
  start = phase;
  memo_select(NULL, NULL);
  speak_live(50u, true);
  connected = false;
  speak_live(50u, true);
  assert(s_memo.state == MEMO_RECORDING);
  connected = true;
  fire_timer(); // The 1 s retry.
  speak_live(50u, true);
  memo_select(NULL, NULL);
  while (s_memo.state == MEMO_SENDING) phone_ack();
  data_length = reference(start, 150u, expect, sizeof(expect));
  assert(stream_length == data_length && memcmp(stream, expect + MEMO_HEADER, data_length) == 0);
  streamed = (InboundTransfer){ .capture_id = s_memo.capture, .generation = s_memo.crc, .total_bytes = s_memo.length,
    .status = HERMES_STATUS_FAILED, .error_code = HERMES_ERROR_MALFORMED };
  memo_receipt(&streamed);
  assert(s_memo.state == MEMO_FAILED && !s_memo.buffer && strstr(s_memo.status, "could not verify"));
  free_heap = 100000u;

  // Watch flash: items go into Data Logging while recording, BUSY every third call.
  memo_up(NULL, NULL);
  assert(s_memo.mode == MEMO_MODE_LOG && persisted_mode == (int32_t)MEMO_MODE_LOG);
  draw();
  log_busy_every = 3u;
  start = phase;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_RECORDING && log_open && s_memo.capacity == MEMO_LOG_RING);
  uint32_t log_capture_id = s_memo.capture;
  speak_live(1500u, false); // 30 s, more than the 16 KiB ring holds at once.
  draw();
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_SENDING || s_memo.state == MEMO_SENT);
  uint32_t log_samples = s_memo.encoder.samples, log_crc = s_memo.crc, log_total = s_memo.length;
  for (int i = 0; i < 1000 && s_memo.state == MEMO_SENDING; i++) fire_timer();
  assert(s_memo.state == MEMO_SENT && !log_open && log_finishes == 1u && log_end_seen && !s_memo.buffer);
  data_length = reference(start, 1500u, expect, sizeof(expect));
  assert(log_length == data_length && log_total == data_length && memcmp(log_data, expect + MEMO_HEADER, data_length) == 0);
  assert(log_capture == log_capture_id && memo_crc32(log_data, log_length) == log_crc);
  assert(memcmp(log_end + MEMO_LOG_HEAD, "HVM1", 4) == 0 && read_le32(log_end, MEMO_LOG_HEAD + 8u) == log_samples);
  assert(read_le32(log_end, MEMO_LOG_HEAD + 16u) == log_crc && read_le32(log_end, MEMO_LOG_HEAD + 20u) == log_total);
  assert(strstr(s_memo.status, "watch storage"));

  // Watch flash full mid-recording: stop, keep what was logged (session finished), explain.
  memo_select(NULL, NULL);
  speak_live(100u, false);
  log_result = DATA_LOGGING_FULL;
  speak_live(10u, false);
  assert(s_memo.state == MEMO_FAILED && !mic_active && !log_open && log_finishes == 2u && !s_memo.buffer);
  assert(strstr(s_memo.status, "refused"));
  log_result = DATA_LOGGING_SUCCESS;
  log_create_fails = true;
  memo_select(NULL, NULL);
  assert(s_memo.state == MEMO_IDLE && !mic_active && !s_memo.buffer && strstr(s_memo.status, "not available"));
  log_create_fails = false;
  // Leaving Hermes while saving finishes the session, so the phone keeps what arrived.
  memo_select(NULL, NULL);
  speak_live(100u, false);
  memo_up(NULL, NULL);
  assert(s_memo.mode == MEMO_MODE_LOG); // UP does not switch mode while recording.

  memo_unload(NULL);
  memo_shutdown();
  assert(!log_open && log_finishes == 3u);
  assert(s_memo.buffer == NULL && s_memo_window == NULL);
  printf("Voice memo watch flow: record, ACK-paced %u-byte blocks, receipts, resend, backoff, "
         "block shrink, priority, memory bounds, mic refusals, interruptions, coverage, DC-free meter, "
         "mode cycling, stream (ring wrap, lag, outage, rejection) and watch flash (busy, full, exit) passed\n", MEMO_BLOCK_BYTES);
  return 0;
}
